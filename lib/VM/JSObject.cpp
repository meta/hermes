/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JSObject.h"

#include "SHUnitExt.h"
#include "hermes/VM/BuildMetadata.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/DecoratedObject.h"
#include "hermes/VM/Domain.h"
#include "hermes/VM/FastArray.h"
#include "hermes/VM/HostModel.h"
#include "hermes/VM/InternalProperty.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSCallSite.h"
#include "hermes/VM/JSCallableProxy.h"
#include "hermes/VM/JSDataView.h"
#include "hermes/VM/JSDate.h"
#include "hermes/VM/JSError.h"
#include "hermes/VM/JSFinalizationRegistry.h"
#include "hermes/VM/JSGeneratorObject.h"
#include "hermes/VM/JSLib.h"
#include "hermes/VM/JSLib/JSLibStorage.h"
#include "hermes/VM/JSMapImpl.h"
#include "hermes/VM/JSObject-inline.h"
#include "hermes/VM/JSProxy.h"
#include "hermes/VM/JSRegExp.h"
#include "hermes/VM/JSRegExpStringIterator.h"
#include "hermes/VM/JSTypedArray.h"
#include "hermes/VM/JSWeakMapImpl.h"
#include "hermes/VM/JSWeakRef.h"
#include "hermes/VM/NativeState.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/PrimitiveBox.h"
#include "hermes/VM/PropertyAccessor.h"
#include "hermes/VM/SingleObject.h"
#include "hermes/VM/StaticHUtils.h"

#include "llvh/ADT/DenseSet.h"
#include "llvh/ADT/ScopeExit.h"

namespace hermes {
namespace vm {

const ObjectVTable JSObject::vt{
    VTable(
        CellKind::JSObjectKind,
        cellSize<JSObject>(),
        /* allowLargeAlloc */ false,
        nullptr,
        nullptr,
        nullptr
#ifdef HERMES_MEMORY_INSTRUMENTATION
        ,
        VTable::HeapSnapshotMetadata{
            HeapSnapshot::NodeType::Object,
            JSObject::_snapshotNameImpl,
            JSObject::_snapshotAddEdgesImpl,
            nullptr,
            JSObject::_snapshotAddLocationsImpl}
#endif
        ),
    JSObject::_getOwnIndexedRangeImpl,
    JSObject::_haveOwnIndexedImpl,
    JSObject::_getOwnIndexedPropertyFlagsImpl,
    JSObject::_getOwnIndexedImpl,
    JSObject::_setOwnIndexedImpl,
    JSObject::_deleteOwnIndexedImpl,
    JSObject::_checkAllOwnIndexedImpl,
};

void JSObjectBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  // This call is just for debugging and consistency purposes.
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<JSObject>());

  const auto *self = static_cast<const JSObject *>(cell);
  mb.setVTable(&JSObject::vt);
  mb.setJitCall(&JSObject::_jitCallImpl);
  mb.addField("class", &self->clazzDoNotAccessDirectly_);
  mb.addField("propStorage", &self->propStorage_);

  // Declare the direct properties.
  static const char *directPropName[JSObject::DIRECT_PROPERTY_SLOTS] = {
      "directProp0",
      "directProp1",
      "directProp2",
      "directProp3",
      "directProp4"};
  for (unsigned i = mb.getJSObjectOverlapSlots();
       i < JSObject::DIRECT_PROPERTY_SLOTS;
       ++i) {
    mb.addField(directPropName[i], self->directProps() + i);
  }
}

size_t JSObject::numOverlapSlotsForCellKind(CellKind k) {
  switch (k) {
#define CELL_JSOBJECT_NAME(name, vmClassName) \
  case CellKind::name##Kind:                  \
    return JSObject::numOverlapSlots<vmClassName>();
#include "hermes/VM/CellKinds.def"
    default:
      hermes_fatal("CellKind does not have a known JS class");
  }
}

void JSObject::setEmptyClassUnsafe(
    JSObject *self,
    Runtime &runtime,
    HiddenClass *clazz) {
  assert(
      self->getClass(runtime)->getNumProperties() == 0 &&
      "object must have no properties");
  assert(clazz->getNumProperties() == 0 && "class must be empty");
  assert(
      clazz->getObjectParent(runtime) == self->getParent(runtime) &&
      "parent must not change");
  self->updateClass(runtime, clazz);
}

PseudoHandle<JSObject> JSObject::create(
    Runtime &runtime,
    Handle<JSObject> parentHandle) {
  return create(
      runtime,
      parentHandle,
      runtime.getHiddenClassForPrototype(*parentHandle, runtime.classJSObject));
}

PseudoHandle<JSObject> JSObject::create(
    Runtime &runtime,
    Handle<JSObject> parentHandle,
    Handle<HiddenClass> clazz,
    unsigned propertyCount) {
  unsigned numIndirectSlots = propertyCount <= DIRECT_PROPERTY_SLOTS
      ? 0
      : propertyCount - DIRECT_PROPERTY_SLOTS;
  auto propStorageSize =
      heapAlignSize(PropStorage::allocationSize(numIndirectSlots));

  PseudoHandle<JSObject> result;
  if (numIndirectSlots == 0) {
    // No indirect storage needed, so just create with no slots.
    auto *cell = runtime.makeAFixed<JSObject>(
        runtime, parentHandle, clazz, GCPointerBase::NoBarriers());
    result = JSObjectInit::initToPseudoHandle(runtime, cell);
  } else if (LLVM_UNLIKELY(propertyCount > maxYoungGenAllocationPropCount())) {
    // The object and property storage together are too big to fit in the young
    // gen together, so allocate them separately.
    auto *cell = runtime.makeAFixed<JSObject>(
        runtime, parentHandle, clazz, GCPointerBase::NoBarriers());
    PseudoHandle<JSObject> self =
        JSObjectInit::initToPseudoHandle(runtime, cell);
    result = runtime.ignoreAllocationFailure(
        JSObject::allocatePropStorage(std::move(self), runtime, propertyCount));
  } else {
    static_assert(
        heapAlignSize(
            PropStorage::allocationSize(
                maxYoungGenAllocationPropCount() - DIRECT_PROPERTY_SLOTS) +
                heapAlignSize(cellSize<JSObject>()) <=
            GC::maxYoungGenAllocationSize()),
        "maxYoungGenAllocationPropCount too large");

    // Otherwise, allocate both parts together.
    // This ensures both the object and the property storage are in the young
    // gen.

    auto [obj, storage] = runtime.make2YoungGenUnsafe<JSObject, PropStorage>(
        cellSize<JSObject>(),
        std::tuple<
            Runtime &,
            Handle<JSObject>,
            Handle<HiddenClass>,
            GCPointerBase::NoBarriers>(
            runtime, parentHandle, clazz, GCPointerBase::NoBarriers()),
        propStorageSize,
        std::tuple{});

    NoAllocScope noAlloc{runtime};

    PropStorage::resizeWithinCapacity(storage, runtime, numIndirectSlots);
    obj->propStorage_.set(runtime, storage, runtime.getHeap());

    result = JSObjectInit::initToPseudoHandle(runtime, obj);
  }

  return result;
}

void JSObject::initializeLazyObject(
    Runtime &runtime,
    Handle<JSObject> lazyObject) {
  assert(lazyObject->isLazy(runtime) && "object must be lazy");
  assert(
      lazyObject->getClass(runtime)->isExtensible() &&
      !lazyObject->getClass(runtime)->getIsCachedUsingEpoch() &&
      "lazy objects must be initialized before changing per-object flags");

  // only functions can be lazy.
  assert(vmisa<Callable>(lazyObject.get()) && "unexpected lazy object");
  Callable::defineLazyProperties(Handle<Callable>::vmcast(lazyObject), runtime);

  assert(!lazyObject->isLazy(runtime) && "object must no longer be lazy");
}

ObjectID JSObject::getObjectID(JSObject *self, Runtime &runtime) {
  if (LLVM_LIKELY(self->flags_.objectID))
    return self->flags_.objectID;

  // Object ID does not yet exist, get next global ID.
  // Use JenkinsHash to avoid IDs being completely sequential.
  self->flags_.objectID = hermes::jenkinsHashUint32(++runtime.objectIDCounter);
  // Make sure it is not zero.
  if (LLVM_UNLIKELY(!self->flags_.objectID))
    --self->flags_.objectID;
  return self->flags_.objectID;
}

CallResult<PseudoHandle<JSObject>> JSObject::getPrototypeOf(
    PseudoHandle<JSObject> selfHandle,
    Runtime &runtime) {
  if (LLVM_LIKELY(!selfHandle->isProxyObject(runtime))) {
    return createPseudoHandle(selfHandle->getParent(runtime));
  }

  return JSProxy::getPrototypeOf(
      runtime.makeHandle(std::move(selfHandle)), runtime);
}

namespace {

CallResult<bool> proxyOpFlags(
    Runtime &runtime,
    PropOpFlags opFlags,
    const char *msg,
    CallResult<bool> res) {
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (!*res && opFlags.getThrowOnError()) {
    return runtime.raiseTypeError(msg);
  }
  return res;
}

} // namespace

CallResult<bool> JSObject::setParent(
    JSObject *self,
    Runtime &runtime,
    JSObject *parent,
    PropOpFlags opFlags) {
  if (LLVM_UNLIKELY(self->isProxyObject(runtime))) {
    return proxyOpFlags(
        runtime,
        opFlags,
        "Object is not extensible.",
        JSProxy::setPrototypeOf(
            runtime.makeHandle(self), runtime, runtime.makeHandle(parent)));
  }
  // ES9 9.1.2
  // 4.
  if (self->getParent(runtime) == parent)
    return true;
  // ES2022 10.4.7 Immutable Prototype Exotic Objects
  // The [[SetPrototypeOf]] for %Object.prototype% is supposed to be
  // SetImmutablePrototype, which returns false and subsequently throws
  // when the argument isn't the same as the existing prototype,
  // which we've already checked above.
  // %Object.prototype% is the only object in the spec which is an immutable
  // prototype exotic object.
  // The global object's parent is host-defined,
  // so we also make the global object be an immutable prototype object
  // because modifying the parent of a global object can lead to bizarre
  // states like Proxies in the parent chain of the global object.
  if (LLVM_UNLIKELY(self == runtime.objectPrototypeRawPtr) ||
      LLVM_UNLIKELY(self == runtime.getGlobal().get())) {
    return runtime.raiseTypeError(
        "Cannot set prototype of immutable prototype object");
  }
  // 5.
  if (!self->isExtensible(runtime)) {
    if (opFlags.getThrowOnError()) {
      return runtime.raiseTypeError("Object is not extensible.");
    } else {
      return false;
    }
  }
  // 6-8. Check for a prototype cycle.
  for (JSObject *cur = parent; cur; cur = cur->getParent(runtime)) {
    if (cur == self) {
      if (opFlags.getThrowOnError()) {
        return runtime.raiseTypeError("Prototype cycle detected");
      } else {
        return false;
      }
    } else if (LLVM_UNLIKELY(cur->isProxyObject(runtime))) {
      // TODO this branch should also be used for module namespace and
      // immutable prototype exotic objects.
      break;
    }
  }

  // Changing the parent may result in adding a setter/readonly property in the
  // parent chain that would prevent adding the property to the cached
  // HiddenClass.
  // We must break the cache, so increment the epoch.
  if (LLVM_UNLIKELY(self->isCachedUsingEpoch(runtime))) {
    runtime.incParentCacheEpoch();
  }

  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
    PinnedValue<JSObject> self;
    PinnedValue<JSObject> parent;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.self = self;
  lv.clazz = lv.self->getClass(runtime);
  lv.parent = parent;

  // 9.
  lv.clazz = HiddenClass::updateObjectParent(
      lv.clazz, runtime, createPseudoHandle(*lv.parent));
  lv.self->updateClass(runtime, *lv.clazz);

  // 10.
  return true;
}

ExecutionStatus JSObject::allocateNewSlotStorage(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SlotIndex newSlotIndex,
    Handle<> valueHandle) {
  // If it is a direct property, just store the value and we are done.
  if (LLVM_LIKELY(newSlotIndex < DIRECT_PROPERTY_SLOTS)) {
    auto shv = SmallHermesValue::encodeHermesValue(*valueHandle, runtime);
    selfHandle->directProps()[newSlotIndex].set(shv, runtime.getHeap());
    return ExecutionStatus::RETURNED;
  }

  // Make the slot index relative to the indirect storage.
  newSlotIndex -= DIRECT_PROPERTY_SLOTS;

  // Allocate a new property storage if not already allocated.
  if (LLVM_UNLIKELY(!selfHandle->propStorage_)) {
    // Allocate new storage.
    assert(newSlotIndex == 0 && "allocated slot must be at end");
    auto arrRes = runtime.ignoreAllocationFailure(
        PropStorage::create(runtime, DEFAULT_PROPERTY_CAPACITY));
    selfHandle->propStorage_.setNonNull(
        runtime, vmcast<PropStorage>(arrRes), runtime.getHeap());
  } else if (auto propStoragePtr = selfHandle->propStorage_.getNonNull(runtime);
             LLVM_UNLIKELY(newSlotIndex >= propStoragePtr->capacity())) {
    // Reallocate the existing one.
    assert(
        newSlotIndex == selfHandle->propStorage_.getNonNull(runtime)->size() &&
        "allocated slot must be at end");

    UsePinnedValueRAII<PropStorage> propStorageUse{runtime.hvStorageTmp};
    propStorageUse = propStoragePtr;
    if (LLVM_UNLIKELY(
            PropStorage::resize(propStorageUse, runtime, newSlotIndex + 1) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    };
    selfHandle->propStorage_.setNonNull(
        runtime, propStorageUse.get(), runtime.getHeap());
  }

  {
    NoAllocScope scope{runtime};
    auto *const propStorage = selfHandle->propStorage_.getNonNull(runtime);
    if (newSlotIndex >= propStorage->size()) {
      assert(
          newSlotIndex == propStorage->size() &&
          "allocated slot must be at end");
      PropStorage::resizeWithinCapacity(propStorage, runtime, newSlotIndex + 1);
    }
  }
  // This must be done after the call to resizeWithinCapacity, since
  // encodeHermesValue may allocate and cause the ArrayStorage to be trimmed.
  auto shv = SmallHermesValue::encodeHermesValue(*valueHandle, runtime);
  // If we don't need to resize, just store it directly.
  selfHandle->propStorage_.getNonNull(runtime)->set(
      newSlotIndex, shv, runtime.getHeap());
  return ExecutionStatus::RETURNED;
}

CallResult<PseudoHandle<>> JSObject::getNamedPropertyValue_RJS(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<JSObject> propObj,
    NamedPropertyDescriptor desc) {
  if (LLVM_LIKELY(!desc.flags.accessor))
    return getNamedSlotValue(propObj, runtime, desc);

  // It's now valid to use the Internal variant because we know it's an
  // accessor.
  auto *accessor = vmcast<PropertyAccessor>(
      getNamedSlotValueUnsafe(propObj.get(), runtime, desc).getObject(runtime));
  if (!accessor->getter)
    return createPseudoHandle(HermesValue::encodeUndefinedValue());

  // Execute the accessor on this object.
  return accessor->getter.getNonNull(runtime)->executeCall0(
      runtime.makeHandle(accessor->getter), runtime, selfHandle);
}

CallResult<PseudoHandle<>> JSObject::getComputedPropertyValueInternal_RJS(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<JSObject> propObj,
    ComputedPropertyDescriptor desc) {
  assert(
      !selfHandle->isProxyObject(runtime) && !propObj->isProxyObject(runtime) &&
      "getComputedPropertyValue_RJS cannot be used with proxy objects");

  if (LLVM_LIKELY(!desc.flags.accessor))
    return createPseudoHandle(getComputedSlotValueUnsafe(
        createPseudoHandle(propObj.get()), runtime, desc));

  auto *accessor = vmcast<PropertyAccessor>(getComputedSlotValueUnsafe(
      createPseudoHandle(propObj.get()), runtime, desc));
  if (!accessor->getter)
    return createPseudoHandle(HermesValue::encodeUndefinedValue());

  // Execute the accessor on this object.
  return accessor->getter.getNonNull(runtime)->executeCall0(
      runtime.makeHandle(accessor->getter), runtime, selfHandle);
}

CallResult<PseudoHandle<>> JSObject::getComputedPropertyValue_RJS(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<JSObject> propObj,
    ComputedPropertyDescriptor desc,
    Handle<> nameValHandle) {
  if (!propObj) {
    return createPseudoHandle(HermesValue::encodeEmptyValue());
  }

  if (LLVM_LIKELY(!desc.flags.proxyObject)) {
    return JSObject::getComputedPropertyValueInternal_RJS(
        selfHandle, runtime, propObj, desc);
  }

  CallResult<Handle<>> keyRes = toPropertyKey(runtime, nameValHandle);
  if (LLVM_UNLIKELY(keyRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  CallResult<bool> hasRes = JSProxy::hasComputed(propObj, runtime, *keyRes);
  if (LLVM_UNLIKELY(hasRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (!*hasRes) {
    return createPseudoHandle(HermesValue::encodeEmptyValue());
  }
  return JSProxy::getComputed(propObj, runtime, *keyRes, selfHandle);
}

CallResult<Handle<JSArray>> JSObject::getOwnPropertyKeys(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    OwnKeysFlags okFlags) {
  assert(
      (okFlags.getIncludeNonSymbols() || okFlags.getIncludeSymbols()) &&
      "Can't exclude symbols and strings");
  if (LLVM_UNLIKELY(
          selfHandle->isLazy(runtime) || selfHandle->isProxyObject(runtime))) {
    if (selfHandle->isProxyObject(runtime)) {
      CallResult<PseudoHandle<JSArray>> proxyRes =
          JSProxy::ownPropertyKeys(selfHandle, runtime, okFlags);
      if (LLVM_UNLIKELY(proxyRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      return runtime.makeHandle(std::move(*proxyRes));
    }
    assert(selfHandle->isLazy(runtime) && "descriptor flags are impossible");
    initializeLazyObject(runtime, selfHandle);
  }

  auto range = getOwnIndexedRange(selfHandle.get(), runtime);

  // Estimate the capacity of the output array.  This estimate is only
  // reasonable for the non-symbol case.
  const uint32_t capacity = okFlags.getIncludeNonSymbols()
      ? (selfHandle->getClass(runtime)->getNumProperties() + range.second -
         range.first)
      : 0;

  auto arrayRes = JSArray::create(runtime, capacity, 0);
  if (LLVM_UNLIKELY(arrayRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<JSArray> array = runtime.makeHandle(std::move(*arrayRes));

  // Optional array of SymbolIDs reported via host object API
  llvh::Optional<Handle<JSArray>> hostObjectSymbols;
  size_t hostObjectSymbolCount = 0;

  // If current object is a host object we need to deduplicate its properties
  llvh::SmallDenseSet<SymbolID::RawType, 16> dedupSet;

  // Output index.
  uint32_t index = 0;

  // Avoid allocating a new handle per element.
  MutableHandle<> tmpHandle{runtime};

  // Number of indexed properties.
  uint32_t numIndexed = 0;

  // Regular properties with names that are array indexes are stashed here, if
  // encountered.
  llvh::SmallVector<uint32_t, 8> indexNames{};

  // Get host object property names
  if (LLVM_UNLIKELY(selfHandle->isHostObject(runtime))) {
    assert(
        range.first == range.second && "Host objects cannot own indexed range");
    // Create a temporary handle to receive the host property names
    MutableHandle<JSArray> hostSymbolsTemp{runtime};
    if (vmcast<HostObject>(selfHandle.get())
            ->getHostPropertyNames(hostSymbolsTemp) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    if ((hostObjectSymbolCount = hostSymbolsTemp->getEndIndex()) != 0) {
      Handle<JSArray> hostSymbols = hostSymbolsTemp;
      hostObjectSymbols = std::move(hostSymbols);
    }
  }

  // Iterate the named properties excluding those which use Symbols.
  if (okFlags.getIncludeNonSymbols()) {
    // Iterate the indexed properties.
    GCScopeMarkerRAII marker{runtime};
    for (auto i = range.first; i != range.second; ++i) {
      auto res = getOwnIndexedPropertyFlags(selfHandle.get(), runtime, i);
      if (!res)
        continue;

      // If specified, check whether it is enumerable.
      if (!okFlags.getIncludeNonEnumerable() && !res->enumerable)
        continue;

      tmpHandle = HermesValue::encodeTrustedNumberValue(i);
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(array, runtime, index++, tmpHandle) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      marker.flush();
    }

    numIndexed = index;

    bool result = HiddenClass::forEachPropertyWhile(
        runtime.makeHandle(selfHandle->getClassGCPtr()),
        runtime,
        [okFlags,
         array,
         hostObjectSymbolCount,
         &index,
         &indexNames,
         &tmpHandle,
         &dedupSet](
            Runtime &runtime, SymbolID id, NamedPropertyDescriptor desc) {
          if (desc.flags.privateName)
            return true;
          if (!isPropertyNamePrimitive(id)) {
            return true;
          }

          // If specified, check whether it is enumerable.
          if (!okFlags.getIncludeNonEnumerable()) {
            if (!desc.flags.enumerable)
              return true;
          }

          // Host properties might overlap with the ones recognized by the
          // hidden class. If we're dealing with a host object then keep track
          // of hidden class properties for the deduplication purposes.
          if (LLVM_UNLIKELY(hostObjectSymbolCount > 0)) {
            dedupSet.insert(id.unsafeGetRaw());
          }

          // Check if this property is an integer index. If it is, we stash it
          // away to deal with it later. This check should be fast since most
          // property names don't start with a digit.
          auto propNameAsIndex = toArrayIndex(
              runtime.getIdentifierTable().getStringView(runtime, id));
          if (LLVM_UNLIKELY(propNameAsIndex)) {
            indexNames.push_back(*propNameAsIndex);
            return true;
          }

          if (okFlags.getKeepSymbols()) {
            tmpHandle = HermesValue::encodeSymbolValue(id);
          } else {
            tmpHandle = HermesValue::encodeStringValue(
                runtime.getStringPrimFromSymbolID(id));
          }
          return JSArray::setElementAt(array, runtime, index++, tmpHandle) !=
              ExecutionStatus::EXCEPTION;
        });

    if (LLVM_UNLIKELY(!result))
      return ExecutionStatus::EXCEPTION;
  }

  // Now iterate the named properties again, including only Symbols.
  if (okFlags.getIncludeSymbols()) {
    MutableHandle<SymbolID> idHandle{runtime};
    bool result = HiddenClass::forEachPropertyWhile(
        runtime.makeHandle(selfHandle->getClassGCPtr()),
        runtime,
        [okFlags, array, &index, &idHandle, hostObjectSymbolCount, &dedupSet](
            Runtime &runtime, SymbolID id, NamedPropertyDescriptor desc) {
          if (desc.flags.privateName)
            return true;
          if (!isSymbolPrimitive(id)) {
            return true;
          }
          // If specified, check whether it is enumerable.
          if (!okFlags.getIncludeNonEnumerable()) {
            if (!desc.flags.enumerable)
              return true;
          }

          // Keep track of hidden class properties for deduplication purposes.
          if (LLVM_UNLIKELY(hostObjectSymbolCount > 0)) {
            dedupSet.insert(id.unsafeGetRaw());
          }

          idHandle = id;
          return JSArray::setElementAt(array, runtime, index++, idHandle) !=
              ExecutionStatus::EXCEPTION;
        });
    if (LLVM_UNLIKELY(!result))
      return ExecutionStatus::EXCEPTION;
  }

  // Iterate over HostObject properties and append them to the array. Do not
  // append duplicates.
  if (LLVM_UNLIKELY(hostObjectSymbols)) {
    GCScopeMarkerRAII marker{runtime};
    for (size_t i = 0; i < hostObjectSymbolCount; ++i) {
      assert(
          (*hostObjectSymbols)->at(runtime, i).isSymbol() &&
          "Host object needs to return array of SymbolIDs");
      marker.flush();
      SymbolID id = (*hostObjectSymbols)->at(runtime, i).getSymbol();
      assert(
          !InternalProperty::isInternal(id) &&
          "host object returned reserved symbol");

      auto success = dedupSet.insert(id.unsafeGetRaw()).second;
      // Skip if it duplicates a hidden class property.
      if (!success) {
        continue;
      }

      if (id.isNotUniqued()) {
        // If not including symbols, symbol id should be uniqued.
        if (!okFlags.getIncludeSymbols()) {
          continue;
        }

        // Return the original symbol value.
        tmpHandle = HermesValue::encodeSymbolValue(id);
      } else {
        // If not including strings, symbol id should not be uniqued.
        if (!okFlags.getIncludeNonSymbols()) {
          continue;
        }

        // We should do this only if it's a string property.
        auto propNameAsIndex = toArrayIndex(
            runtime.getIdentifierTable().getStringView(runtime, id));
        if (LLVM_UNLIKELY(propNameAsIndex)) {
          indexNames.push_back(*propNameAsIndex);
          continue;
        }

        // Return string value to caller.
        tmpHandle = HermesValue::encodeStringValue(
            runtime.getStringPrimFromSymbolID(id));
      }
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(array, runtime, index++, tmpHandle) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
    }
  }

  // The end (exclusive) of the named properties.
  uint32_t endNamed = index;

  // Properly set the length of the array.
  auto cr = JSArray::setLength(
      array, runtime, endNamed + indexNames.size(), PropOpFlags{});
  (void)cr;
  assert(
      cr != ExecutionStatus::EXCEPTION && *cr && "JSArray::setLength() failed");

  // If we have no index-like names, we are done.
  if (LLVM_LIKELY(indexNames.empty()))
    return array;

  // In the unlikely event that we encountered index-like names, we need to sort
  // them and merge them with the real indexed properties. Note that it is
  // guaranteed that there are no clashes.
  std::sort(indexNames.begin(), indexNames.end());

  // Also make space for the new elements by shifting all the named properties
  // to the right. First, resize the array.
  JSArray::setStorageEndIndex(array, runtime, endNamed + indexNames.size());

  // Shift the non-index property names. The region [numIndexed..endNamed) is
  // moved to [numIndexed+indexNames.size()..array->size()).
  // TODO: optimize this by implementing memcpy-like functionality in ArrayImpl.
  for (uint32_t last = endNamed, toLast = array->getEndIndex();
       last != numIndexed;) {
    --last;
    --toLast;
    tmpHandle = array->at(runtime, last).unboxToHV(runtime);
    if (LLVM_UNLIKELY(
            JSArray::setElementAt(array, runtime, toLast, tmpHandle) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }

  // Now we need to merge the indexes in indexNames and the array
  // [0..numIndexed). We start from the end and copy the larger element from
  // either array.
  // 1+ the destination position to copy into.
  for (uint32_t toLast = numIndexed + indexNames.size(),
                indexNamesLast = indexNames.size();
       toLast != 0;) {
    if (numIndexed) {
      uint32_t a =
          (uint32_t)array->at(runtime, numIndexed - 1).getNumber(runtime);
      uint32_t b;

      if (indexNamesLast && (b = indexNames[indexNamesLast - 1]) > a) {
        tmpHandle = HermesValue::encodeTrustedNumberValue(b);
        --indexNamesLast;
      } else {
        tmpHandle = HermesValue::encodeTrustedNumberValue(a);
        --numIndexed;
      }
    } else {
      assert(indexNamesLast && "prematurely ran out of source values");
      tmpHandle =
          HermesValue::encodeTrustedNumberValue(indexNames[indexNamesLast - 1]);
      --indexNamesLast;
    }

    --toLast;
    if (LLVM_UNLIKELY(
            JSArray::setElementAt(array, runtime, toLast, tmpHandle) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }

  return array;
}

/// Convert a value to string unless already converted
/// \param nameValHandle [Handle<>] the value to convert
/// \param str [MutableHandle<StringPrimitive>] the string is stored
///   there. Must be initialized to null initially.
#define LAZY_TO_STRING(runtime, nameValHandle, str)       \
  do {                                                    \
    if (!str) {                                           \
      auto status = toString_RJS(runtime, nameValHandle); \
      assert(                                             \
          status != ExecutionStatus::EXCEPTION &&         \
          "toString() of primitive cannot fail");         \
      str = status->get();                                \
    }                                                     \
  } while (0)

/// Convert a value to an identifier unless already converted
/// \param nameValHandle [Handle<>] the value to convert
/// \param id [SymbolID] the identifier is stored there. Must be initialized
///   to INVALID_IDENTIFIER_ID initially.
#define LAZY_TO_IDENTIFIER(runtime, nameValHandle, id)          \
  do {                                                          \
    if (id.isInvalid()) {                                       \
      CallResult<Handle<SymbolID>> idRes =                      \
          valueToSymbolID(runtime, nameValHandle);              \
      if (LLVM_UNLIKELY(idRes == ExecutionStatus::EXCEPTION)) { \
        return ExecutionStatus::EXCEPTION;                      \
      }                                                         \
      id = **idRes;                                             \
    }                                                           \
  } while (0)

/// Convert a value to array index, if possible.
/// \param nameValHandle [Handle<>] the value to convert
/// \param str [StringPrimitive *] the string is stored
///   there. Must be initialized to null initially.
/// \param arrayIndex [OptValue<uint32_t>] the array index is stored
///   there.
#define TO_ARRAY_INDEX(runtime, nameValHandle, str, arrayIndex) \
  do {                                                          \
    assert(str == nullptr && "must be null");                   \
    arrayIndex = toArrayIndexFastPath(*nameValHandle);          \
    if (!arrayIndex && !nameValHandle->isSymbol()) {            \
      LAZY_TO_STRING(runtime, nameValHandle, str);              \
      arrayIndex = toArrayIndex(str);                           \
    }                                                           \
  } while (0)

/// \return true if the flags of a new property make it suitable for indexed
///   storage. All new indexed properties are enumerable, writable and
///   configurable and have no accessors.
static bool canNewPropertyBeIndexed(DefinePropertyFlags dpf) {
  return dpf.setEnumerable && dpf.enumerable && dpf.setWritable &&
      dpf.writable && dpf.setConfigurable && dpf.configurable &&
      !dpf.setSetter && !dpf.setGetter;
}

struct JSObject::Helper {
 public:
  LLVM_ATTRIBUTE_ALWAYS_INLINE
  static SHObjectFlags &flags(JSObject *self) {
    return self->flags_;
  }

  LLVM_ATTRIBUTE_ALWAYS_INLINE
  static OptValue<PropertyFlags>
  getOwnIndexedPropertyFlags(JSObject *self, Runtime &runtime, uint32_t index) {
    return JSObject::getOwnIndexedPropertyFlags(self, runtime, index);
  }

  LLVM_ATTRIBUTE_ALWAYS_INLINE
  static NamedPropertyDescriptor &castToNamedPropertyDescriptorRef(
      ComputedPropertyDescriptor &desc) {
    return desc.castToNamedPropertyDescriptorRef();
  }
};

namespace {

/// ES5.1 8.12.1.

/// A helper which takes a SymbolID which caches the conversion of
/// nameValHandle if it's needed.  It should be default constructed,
/// and may or may not be set.  This has been measured to be a useful
/// perf win.  Note that always_inline seems to be ignored on static
/// methods, so this function has to be local to the cpp file in order
/// to be inlined for the perf win.
///
/// \p tmpSymbolStorageUnused is not actually used. We pass it around to
/// ensure that it is still alive from the call that needed it.
LLVM_ATTRIBUTE_ALWAYS_INLINE
inline CallResult<bool> getOwnComputedPrimitiveDescriptorImpl(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<> nameValHandle,
    JSObject::IgnoreProxy ignoreProxy,
    SymbolID &id,
    ComputedPropertyDescriptor &desc) {
  assert(
      !nameValHandle->isObject() &&
      "nameValHandle passed to "
      "getOwnComputedPrimitiveDescriptor "
      "cannot be an object");

  // Try the fast paths first if we have "fast" index properties and the
  // property name is an obvious index.
  if (auto arrayIndex = toArrayIndexFastPath(*nameValHandle)) {
    if (selfHandle->hasFastIndexProperties(runtime)) {
      auto res = JSObject::Helper::getOwnIndexedPropertyFlags(
          selfHandle.get(), runtime, *arrayIndex);
      if (res) {
        // This a valid array index, residing in our indexed storage.
        desc.flags = *res;
        desc.flags.indexed = 1;
        desc.slot = *arrayIndex;
        return true;
      }

      // This a valid array index, but we don't have it in our indexed storage,
      // and we don't have index-like named properties.
      return false;
    }

    if (!selfHandle->getClass(runtime)->getHasIndexLikeProperties() &&
        !selfHandle->isHostObject(runtime) && !selfHandle->isLazy(runtime) &&
        !selfHandle->isProxyObject(runtime)) {
      // Early return to handle the case where an object definitely has no
      // index-like properties. This avoids allocating a new StringPrimitive and
      // uniquing it below.
      return false;
    }
  }

  // Convert the string to a SymbolID
  LAZY_TO_IDENTIFIER(runtime, nameValHandle, id);

  // Look for a named property with this name.
  if (JSObject::getOwnNamedDescriptor(
          selfHandle,
          runtime,
          id,
          JSObject::Helper::castToNamedPropertyDescriptorRef(desc))) {
    return true;
  }

  if (LLVM_LIKELY(
          !selfHandle->hasIndexedStorage(runtime) &&
          !selfHandle->isLazy(runtime) &&
          !selfHandle->isProxyObject(runtime))) {
    return false;
  }

  // If we have indexed storage, perform potentially expensive conversions
  // to array index and check it.
  if (selfHandle->hasIndexedStorage(runtime)) {
    // If the name is a valid integer array index, store it here.
    OptValue<uint32_t> arrayIndex;

    // Try to convert the property name to an array index.
    {
      StringPrimitive *strPrim = nullptr;
      TO_ARRAY_INDEX(runtime, nameValHandle, strPrim, arrayIndex);
    }

    if (arrayIndex) {
      auto res = JSObject::Helper::getOwnIndexedPropertyFlags(
          selfHandle.get(), runtime, *arrayIndex);
      if (res) {
        desc.flags = *res;
        desc.flags.indexed = 1;
        desc.slot = *arrayIndex;
        return true;
      }
    }
    return false;
  }

  if (selfHandle->isLazy(runtime)) {
    JSObject::initializeLazyObject(runtime, selfHandle);
    return JSObject::getOwnComputedPrimitiveDescriptor(
        selfHandle, runtime, nameValHandle, ignoreProxy, desc);
  }

  assert(
      selfHandle->isProxyObject(runtime) && "descriptor flags are impossible");
  if (ignoreProxy == JSObject::IgnoreProxy::Yes) {
    return false;
  }
  return JSProxy::getOwnProperty(
      selfHandle, runtime, nameValHandle, desc, nullptr);
}

} // namespace

CallResult<bool> JSObject::getOwnComputedPrimitiveDescriptor(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<> nameValHandle,
    JSObject::IgnoreProxy ignoreProxy,
    ComputedPropertyDescriptor &desc) {
  SymbolID id{};

  return getOwnComputedPrimitiveDescriptorImpl(
      selfHandle, runtime, nameValHandle, ignoreProxy, id, desc);
}

CallResult<bool> JSObject::getOwnComputedDescriptor(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<> nameValHandle,
    ComputedPropertyDescriptor &desc) {
  auto converted = toPropertyKeyIfObject(runtime, nameValHandle);
  if (LLVM_UNLIKELY(converted == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return JSObject::getOwnComputedPrimitiveDescriptor(
      selfHandle, runtime, *converted, IgnoreProxy::No, desc);
}

CallResult<bool> JSObject::getOwnComputedDescriptor(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<> nameValHandle,
    ComputedPropertyDescriptor &desc,
    MutableHandle<> valueOrAccessor) {
  auto converted = toPropertyKeyIfObject(runtime, nameValHandle);
  if (LLVM_UNLIKELY(converted == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // The proxy is ignored here so we can avoid calling
  // JSProxy::getOwnProperty twice on proxies, since
  // getOwnComputedPrimitiveDescriptor doesn't pass back the
  // valueOrAccessor.
  CallResult<bool> res = JSObject::getOwnComputedPrimitiveDescriptor(
      selfHandle, runtime, *converted, IgnoreProxy::Yes, desc);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (*res) {
    // This is safe because we passed IgnoreProxy::Yes above,
    // meaning that this will return false in proxy cases.
    valueOrAccessor.set(getComputedSlotValueUnsafe(
        createPseudoHandle(selfHandle.get()), runtime, desc));
    return true;
  }
  if (LLVM_UNLIKELY(selfHandle->isProxyObject(runtime))) {
    return JSProxy::getOwnProperty(
        selfHandle, runtime, nameValHandle, desc, &valueOrAccessor);
  }
  return false;
}

JSObject *JSObject::getNamedDescriptorUnsafe(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SymbolID name,
    NamedPropertyDescriptor &desc) {
  if (findProperty(selfHandle, runtime, name, desc))
    return *selfHandle;

  // Check here for host object flag.  This means that "normal" own
  // properties above win over host-defined properties, but there's no
  // cost imposed on own property lookups.  This should do what we
  // need in practice, and we can define host vs js property
  // disambiguation however we want.  This is here in order to avoid
  // impacting perf for the common case where an own property exists
  // in normal storage.
  if (LLVM_UNLIKELY(selfHandle->isHostObject(runtime))) {
    desc.flags.hostObject = true;
    desc.flags.writable = true;
    desc.slot = name.unsafeGetRaw();
    return *selfHandle;
  }

  if (LLVM_UNLIKELY(selfHandle->isLazy(runtime))) {
    assert(
        !selfHandle->isProxyObject(runtime) &&
        "Proxy objects should never be lazy");
    // Initialize the object and perform the lookup again.
    JSObject::initializeLazyObject(runtime, selfHandle);

    if (findProperty(selfHandle, runtime, name, desc))
      return *selfHandle;
  }

  if (LLVM_UNLIKELY(selfHandle->isProxyObject(runtime))) {
    desc.flags.proxyObject = true;
    desc.slot = name.unsafeGetRaw();
    return *selfHandle;
  }

  if (selfHandle->getParentGCPtr(runtime)) {
    MutableHandle<JSObject> mutableSelfHandle{
        runtime, selfHandle->getParentNonNull(runtime)};

    do {
      GCScopeMarkerRAII marker{runtime};
      // Check the most common case first, at the cost of some code duplication.
      if (LLVM_LIKELY(
              !mutableSelfHandle->isLazy(runtime) &&
              !mutableSelfHandle->isHostObject(runtime) &&
              !mutableSelfHandle->isProxyObject(runtime))) {
      findProp:
        if (findProperty(mutableSelfHandle, runtime, name, desc)) {
          assert(
              !selfHandle->isProxyObject(runtime) &&
              "Proxy object parents should never have own properties");
          return *mutableSelfHandle;
        }
      } else if (LLVM_UNLIKELY(mutableSelfHandle->isLazy(runtime))) {
        JSObject::initializeLazyObject(runtime, mutableSelfHandle);
        goto findProp;
      } else if (LLVM_UNLIKELY(mutableSelfHandle->isHostObject(runtime))) {
        desc.flags.hostObject = true;
        desc.flags.writable = true;
        desc.slot = name.unsafeGetRaw();
        return *mutableSelfHandle;
      } else {
        assert(
            mutableSelfHandle->isProxyObject(runtime) &&
            "descriptor flags are impossible");
        desc.flags.proxyObject = true;
        desc.slot = name.unsafeGetRaw();
        return *mutableSelfHandle;
      }
    } while ((mutableSelfHandle = mutableSelfHandle->getParent(runtime)));
  }

  return nullptr;
}

ExecutionStatus JSObject::getComputedPrimitiveDescriptor(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<> nameValHandle,
    MutableHandle<JSObject> propObj,
    ComputedPropertyDescWithSymStorage &desc) {
  assert(
      !nameValHandle->isObject() &&
      "nameValHandle passed to "
      "getComputedPrimitiveDescriptor cannot "
      "be an object");

  propObj.castAndSetHermesValue<JSObject>(selfHandle.getHermesValue());

  SymbolID id{};

  GCScopeMarkerRAII marker{runtime};
  do {
    // A proxy is ignored here so we can check the bit later and
    // return it back to the caller for additional processing.

    CallResult<bool> res = getOwnComputedPrimitiveDescriptorImpl(
        propObj, runtime, nameValHandle, IgnoreProxy::Yes, id, desc);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (*res) {
      return ExecutionStatus::RETURNED;
    }

    if (LLVM_UNLIKELY(propObj->isHostObject(runtime))) {
      desc.flags.hostObject = true;
      desc.flags.writable = true;
      desc.slot = id.unsafeGetRaw();
      desc._tmpSymbolStorage.set(id);
      return ExecutionStatus::RETURNED;
    }
    if (LLVM_UNLIKELY(propObj->isProxyObject(runtime))) {
      desc.flags.proxyObject = true;
      desc.slot = id.unsafeGetRaw();
      desc._tmpSymbolStorage.set(id);
      return ExecutionStatus::RETURNED;
    }
    // This isn't a proxy, so use the faster getParent() instead of
    // getPrototypeOf.
    propObj.set(propObj->getParent(runtime));
    // Flush at the end of the loop to allow first iteration to be as fast as
    // possible.
    marker.flush();
  } while (propObj);
  return ExecutionStatus::RETURNED;
}

ExecutionStatus JSObject::getComputedDescriptor(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<> nameValHandle,
    MutableHandle<JSObject> propObj,
    ComputedPropertyDescWithSymStorage &desc) {
  auto converted = toPropertyKeyIfObject(runtime, nameValHandle);
  if (LLVM_UNLIKELY(converted == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return getComputedPrimitiveDescriptor(
      selfHandle, runtime, *converted, propObj, desc);
}

CallResult<PseudoHandle<>> JSObject::getNamedWithReceiver_RJS(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SymbolID name,
    Handle<> receiver,
    PropOpFlags opFlags,
    ReadPropertyCacheEntry *cacheEntry) {
  NamedPropertyDescriptor desc;
  // Locate the descriptor. propObj contains the object which may be anywhere
  // along the prototype chain.
  JSObject *propObj = getNamedDescriptorUnsafe(selfHandle, runtime, name, desc);
  if (!propObj) {
    if (LLVM_UNLIKELY(opFlags.getMustExist())) {
      return runtime.raiseReferenceError(
          TwineChar16("Property '") +
          runtime.getIdentifierTable().getStringViewForDev(runtime, name) +
          "' doesn't exist");
    }
    return createPseudoHandle(HermesValue::encodeUndefinedValue());
  }

  if (LLVM_LIKELY(
          !desc.flags.accessor && !desc.flags.hostObject &&
          !desc.flags.proxyObject)) {
    // The object must not be a proxy or HostObject, given that the descriptor
    // does not indicate them.
    assert(
        !selfHandle->isProxyObject(runtime) &&
        !selfHandle->isHostObject(runtime));
    // The object cannot be lazy after a lookup has occurred.
    assert(!selfHandle->isLazy(runtime));

    // Populate the cache if requested.
    if (cacheEntry && desc.slot <= ReadPropertyCacheEntry::kMaxSlot &&
        !propObj->getClass(runtime)->isDictionaryNoCache()) {
      cacheEntry->clazz = propObj->getClassGCPtr();
      cacheEntry->setSlot(desc.slot);
      if (selfHandle->getParent(runtime) == propObj &&
          !selfHandle->getClass(runtime)->isDictionary()) {
        // Property found on an object in the prototype chain.  The proto
        // cache only works for the immediate proto of the the object,
        // so don't cache for deeper prototypes.  We also don't cache
        // if the object HC is a dictionary; those may gain properties without
        // changing the HC value, which breaks the "negative caching" of
        // the object HC.  Note that own-property caching can use
        // a dictionary HC, as long as it hasn't had any properties deleted (or
        // property flags changed) -- hence the isDictionaryNoCache test above.
        // But for the negative caching we do here, we have to exempt all
        // dictionaries, since adding a property could mean that that a
        // subsequent execution should get the value from the object rather than
        // the prototype.
        cacheEntry->negMatchClazz = selfHandle->getClassGCPtr();
      } else {
        cacheEntry->negMatchClazz = CompressedPointer(nullptr);
      }
    }
    return createPseudoHandle(
        getNamedSlotValueUnsafe(propObj, runtime, desc).unboxToHV(runtime));
  }

  if (desc.flags.accessor) {
    auto *accessor = vmcast<PropertyAccessor>(
        getNamedSlotValueUnsafe(propObj, runtime, desc).getPointer(runtime));
    if (!accessor->getter)
      return createPseudoHandle(HermesValue::encodeUndefinedValue());

    // Execute the accessor on this object.
    return Callable::executeCall0(
        runtime.makeHandle(accessor->getter), runtime, receiver);
  } else if (desc.flags.hostObject) {
    auto res = vmcast<HostObject>(propObj)->get(name);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return createPseudoHandle(*res);
  } else {
    assert(desc.flags.proxyObject && "descriptor flags are impossible");
    return JSProxy::getNamed(
        runtime.makeHandle(propObj), runtime, name, receiver);
  }
}

CallResult<PseudoHandle<>> JSObject::getNamedOrIndexed(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SymbolID name,
    PropOpFlags opFlags) {
  if (LLVM_UNLIKELY(selfHandle->hasIndexedStorage(runtime))) {
    // Note that getStringView can be satisfied without materializing the
    // Identifier.
    const auto strView =
        runtime.getIdentifierTable().getStringView(runtime, name);
    if (auto nameAsIndex = toArrayIndex(strView)) {
      return getComputed_RJS(
          selfHandle,
          runtime,
          runtime.makeHandle(
              HermesValue::encodeTrustedNumberValue(*nameAsIndex)));
    }
    // Here we have indexed properties but the symbol was not index-like.
    // Fall through to getNamed().
  }
  return getNamed_RJS(selfHandle, runtime, name, opFlags);
}

CallResult<PseudoHandle<>> JSObject::getComputedWithReceiver_RJS(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<> nameValHandle,
    Handle<> receiver) {
  // Try the fast-path first: no "index-like" properties and the "name" already
  // is a valid integer index.
  if (selfHandle->hasFastIndexProperties(runtime)) {
    if (auto arrayIndex = toArrayIndexFastPath(*nameValHandle)) {
      // Do we have this value present in our array storage? If so, return it.
      PseudoHandle<> ourValue =
          createPseudoHandle(getOwnIndexed(selfHandle, runtime, *arrayIndex));
      if (LLVM_LIKELY(!ourValue->isEmpty()))
        return ourValue;
    }
  }

  // If nameValHandle is an object, we should convert it to string now,
  // because toString may have side-effect, and we want to do this only
  // once.
  auto converted = toPropertyKeyIfObject(runtime, nameValHandle);
  if (LLVM_UNLIKELY(converted == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto nameValPrimitiveHandle = *converted;

  ComputedPropertyDescWithSymStorage desc{MutableHandle<SymbolID>{runtime}};

  // Locate the descriptor. propObj contains the object which may be anywhere
  // along the prototype chain.
  MutableHandle<JSObject> propObj{runtime};
  if (LLVM_UNLIKELY(
          getComputedPrimitiveDescriptor(
              selfHandle, runtime, nameValPrimitiveHandle, propObj, desc) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (!propObj)
    return createPseudoHandle(HermesValue::encodeUndefinedValue());

  if (LLVM_LIKELY(
          !desc.flags.accessor && !desc.flags.hostObject &&
          !desc.flags.proxyObject)) {
    return createPseudoHandle(getComputedSlotValueUnsafe(
        createPseudoHandle(propObj.get()), runtime, desc));
  }

  if (desc.flags.accessor) {
    auto *accessor = vmcast<PropertyAccessor>(getComputedSlotValueUnsafe(
        createPseudoHandle(propObj.get()), runtime, desc));
    if (!accessor->getter)
      return createPseudoHandle(HermesValue::encodeUndefinedValue());

    // Execute the accessor on this object.
    return accessor->getter.getNonNull(runtime)->executeCall0(
        runtime.makeHandle(accessor->getter), runtime, receiver);
  } else if (desc.flags.hostObject) {
    SymbolID id{};
    LAZY_TO_IDENTIFIER(runtime, nameValPrimitiveHandle, id);
    auto propRes = vmcast<HostObject>(propObj.get())->get(id);
    if (propRes == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    return createPseudoHandle(*propRes);
  } else {
    assert(desc.flags.proxyObject && "descriptor flags are impossible");
    CallResult<Handle<>> key = toPropertyKey(runtime, nameValPrimitiveHandle);
    if (key == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    return JSProxy::getComputed(propObj, runtime, *key, receiver);
  }
}

CallResult<bool> JSObject::hasNamed(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SymbolID name) {
  NamedPropertyDescriptor desc;
  JSObject *propObj = getNamedDescriptorUnsafe(selfHandle, runtime, name, desc);
  if (propObj == nullptr) {
    return false;
  }
  if (LLVM_UNLIKELY(desc.flags.proxyObject)) {
    return JSProxy::hasNamed(runtime.makeHandle(propObj), runtime, name);
  }
  return true;
}

CallResult<bool> JSObject::hasNamedOrIndexed(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SymbolID name) {
  if (LLVM_UNLIKELY(selfHandle->hasIndexedStorage(runtime))) {
    const auto strView =
        runtime.getIdentifierTable().getStringView(runtime, name);
    if (auto nameAsIndex = toArrayIndex(strView)) {
      if (haveOwnIndexed(selfHandle.get(), runtime, *nameAsIndex)) {
        return true;
      }
      if (selfHandle->hasFastIndexProperties(runtime)) {
        return false;
      }
    }
    // Here we have indexed properties but the symbol was not stored in the
    // indexedStorage.
    // Fall through to getNamed().
  }
  return hasNamed(selfHandle, runtime, name);
}

CallResult<bool> JSObject::hasComputed(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<> nameValHandle) {
  // Try the fast-path first: no "index-like" properties and the "name" already
  // is a valid integer index.
  if (selfHandle->hasFastIndexProperties(runtime)) {
    if (auto arrayIndex = toArrayIndexFastPath(*nameValHandle)) {
      // Do we have this value present in our array storage? If so, return true.
      if (haveOwnIndexed(selfHandle.get(), runtime, *arrayIndex)) {
        return true;
      }
    }
  }

  // If nameValHandle is an object, we should convert it to string now,
  // because toString may have side-effect, and we want to do this only
  // once.
  auto converted = toPropertyKeyIfObject(runtime, nameValHandle);
  if (LLVM_UNLIKELY(converted == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto nameValPrimitiveHandle = *converted;

  ComputedPropertyDescWithSymStorage desc{MutableHandle<SymbolID>{runtime}};
  MutableHandle<JSObject> propObj{runtime};
  if (getComputedPrimitiveDescriptor(
          selfHandle, runtime, nameValPrimitiveHandle, propObj, desc) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  if (!propObj) {
    return false;
  }
  if (LLVM_UNLIKELY(desc.flags.proxyObject)) {
    CallResult<Handle<>> key = toPropertyKey(runtime, nameValPrimitiveHandle);
    if (key == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    return JSProxy::hasComputed(propObj, runtime, *key);
  }
  // For compatibility with polyfills we want to pretend that all HostObject
  // properties are "own" properties in 'in'. Since there is no way to check for
  // a HostObject property, we must always assume success. In practice the
  // property name would have been obtained from enumerating the properties in
  // JS code that looks something like this:
  //    for(key in hostObj) {
  //      if (key in hostObj)
  //        ...
  //    }
  return true;
}

static ExecutionStatus raiseErrorForOverridingStaticBuiltin(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<SymbolID> name) {
  Handle<StringPrimitive> methodNameHnd =
      runtime.makeHandle(runtime.getStringPrimFromSymbolID(name.get()));
  // If the 'name' property does not exist or is an accessor, we don't display
  // the name.
  NamedPropertyDescriptor desc;
  auto *obj = JSObject::getNamedDescriptorPredefined(
      selfHandle, runtime, Predefined::name, desc);
  assert(
      !selfHandle->isProxyObject(runtime) &&
      "raiseErrorForOverridingStaticBuiltin cannot be used with proxy objects");

  if (!obj || desc.flags.accessor) {
    return runtime.raiseTypeError(
        TwineChar16("Attempting to override read-only builtin method '") +
        TwineChar16(methodNameHnd.get()) + "'");
  }

  // Display the name property of the builtin object if it is a string.
  auto objNameRes =
      JSObject::getNamedSlotValue(createPseudoHandle(obj), runtime, desc);
  if (LLVM_UNLIKELY(objNameRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  PseudoHandle<StringPrimitive> objName =
      PseudoHandle<StringPrimitive>::dyn_vmcast(std::move(*objNameRes));
  if (!objName) {
    return runtime.raiseTypeError(
        TwineChar16("Attempting to override read-only builtin method '") +
        TwineChar16(methodNameHnd.get()) + "'");
  }

  return runtime.raiseTypeError(
      TwineChar16("Attempting to override read-only builtin method '") +
      TwineChar16(objName.get()) + "." + TwineChar16(methodNameHnd.get()) +
      "'");
}

CallResult<bool> JSObject::putNamedWithReceiver_RJS(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SymbolID name,
    Handle<> valueHandle,
    Handle<> receiver,
    PropOpFlags opFlags,
    RuntimeModule *runtimeModule,
    SHUnit *unit,
    WritePropertyCacheEntry *cacheEntry) {
  NamedPropertyDescriptor desc;

  // Look for the property in this object or along the prototype chain.
  // `name` will not be freed before this function returns,
  // so it will outlive the lifetime of `desc`.
  JSObject *propObj = getNamedDescriptorUnsafe(selfHandle, runtime, name, desc);

  // If the property exists (or, we hit a proxy/hostobject on the way
  // up the chain)
  if (propObj) {
    // Get the simple case out of the way: If the property already
    // exists on selfHandle, is not an accessor, selfHandle and
    // receiver are the same, selfHandle is not a host
    // object/proxy/internal setter, and the property is writable,
    // just write into the same slot.

    bool isSameObj = *selfHandle == propObj &&
        selfHandle.getHermesValue().getRaw() == receiver->getRaw();

    if (LLVM_LIKELY(
            isSameObj && !desc.flags.accessor && !desc.flags.internalSetter &&
            !desc.flags.hostObject && !desc.flags.proxyObject &&
            desc.flags.writable)) {
      auto shv = SmallHermesValue::encodeHermesValue(*valueHandle, runtime);
      setNamedSlotValueUnsafe(*selfHandle, runtime, desc, shv);
      return true;
    }

    if (LLVM_UNLIKELY(desc.flags.accessor)) {
      auto *accessor = vmcast<PropertyAccessor>(
          getNamedSlotValueUnsafe(propObj, runtime, desc).getObject(runtime));

      // If it is a read-only accessor, fail.
      if (!accessor->setter) {
        if (opFlags.getThrowOnError()) {
          return runtime.raiseTypeError(
              TwineChar16("Cannot assign to property '") +
              runtime.getIdentifierTable().getStringViewForDev(runtime, name) +
              "' which has only a getter");
        }
        return false;
      }

      // Execute the accessor on this object.
      if (accessor->setter.getNonNull(runtime)->executeCall1(
              runtime.makeHandle(accessor->setter),
              runtime,
              receiver,
              *valueHandle) == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      return true;
    }

    if (LLVM_UNLIKELY(desc.flags.proxyObject)) {
      CallResult<bool> setRes = JSProxy::setNamed(
          runtime.makeHandle(propObj), runtime, name, valueHandle, receiver);
      if (LLVM_UNLIKELY(setRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (!*setRes && opFlags.getThrowOnError()) {
        return runtime.raiseTypeError(
            TwineChar16("Proxy set returned false for property '") +
            runtime.getIdentifierTable().getStringView(runtime, name) + "'");
      }
      return setRes;
    }

    if (LLVM_UNLIKELY(!desc.flags.writable)) {
      if (desc.flags.staticBuiltin) {
        return raiseErrorForOverridingStaticBuiltin(
            selfHandle, runtime, runtime.makeHandle(name));
      }
      if (opFlags.getThrowOnError()) {
        return runtime.raiseTypeError(
            TwineChar16("Cannot assign to read-only property '") +
            runtime.getIdentifierTable().getStringViewForDev(runtime, name) +
            "'");
      }
      return false;
    }

    if (isSameObj && desc.flags.internalSetter) {
      return internalSetter(
          selfHandle, runtime, name, desc, valueHandle, opFlags);
    }
  }

  // The property does not exist as an conventional own property on
  // this object.

  MutableHandle<JSObject> receiverHandle{runtime, *selfHandle};
  if (selfHandle.getHermesValue().getRaw() != receiver->getRaw() ||
      receiverHandle->isHostObject(runtime) ||
      receiverHandle->isProxyObject(runtime)) {
    if (selfHandle.getHermesValue().getRaw() != receiver->getRaw()) {
      receiverHandle = dyn_vmcast<JSObject>(*receiver);
    }
    if (!receiverHandle) {
      return false;
    }

    if (getOwnNamedDescriptor(receiverHandle, runtime, name, desc)) {
      if (LLVM_UNLIKELY(desc.flags.accessor || !desc.flags.writable)) {
        return false;
      }

      assert(
          !receiverHandle->isHostObject(runtime) &&
          !receiverHandle->isProxyObject(runtime) &&
          "getOwnNamedDescriptor never sets hostObject or proxyObject flags");
      auto shv = SmallHermesValue::encodeHermesValue(*valueHandle, runtime);
      setNamedSlotValueUnsafe(*receiverHandle, runtime, desc, shv);
      return true;
    }

    // Now deal with host and proxy object cases.  We need to call
    // getOwnComputedPrimitiveDescriptor because it knows how to call
    // the [[getOwnProperty]] Proxy impl if needed.
    if (LLVM_UNLIKELY(
            receiverHandle->isHostObject(runtime) ||
            receiverHandle->isProxyObject(runtime))) {
      if (receiverHandle->isHostObject(runtime)) {
        return vmcast<HostObject>(receiverHandle.get())
            ->set(name, *valueHandle);
      }
      ComputedPropertyDescriptor desc;
      // getOwnComputedPrimitiveDescriptor and JSProxy::defineOwnProperty expect
      // the key to be passed in as a primitive string value rather than a
      // symbol, if it actually did come from a string.
      Handle<> nameValHandle = name.isUniqued()
          ? runtime.makeHandle(
                HermesValue::encodeStringValue(
                    runtime.getStringPrimFromSymbolID(name)))
          : runtime.makeHandle(name);
      CallResult<bool> descDefinedRes = getOwnComputedPrimitiveDescriptor(
          receiverHandle, runtime, nameValHandle, IgnoreProxy::No, desc);
      if (LLVM_UNLIKELY(descDefinedRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      DefinePropertyFlags dpf;
      if (*descDefinedRes) {
        dpf.setValue = 1;
      } else {
        dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
      }
      return JSProxy::defineOwnProperty(
          receiverHandle, runtime, nameValHandle, dpf, valueHandle, opFlags);
    }
  }

  // Does the caller require it to exist?
  if (LLVM_UNLIKELY(opFlags.getMustExist())) {
    return runtime.raiseReferenceError(
        TwineChar16("Property '") +
        runtime.getIdentifierTable().getStringViewForDev(runtime, name) +
        "' doesn't exist");
  }

  // Add a new property.

  return addOwnProperty(
      receiverHandle,
      runtime,
      name,
      DefinePropertyFlags::getDefaultNewPropertyFlags(),
      valueHandle,
      opFlags,
      runtimeModule,
      unit,
      cacheEntry);
}

CallResult<bool> JSObject::putNamedOrIndexed(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SymbolID name,
    Handle<> valueHandle,
    PropOpFlags opFlags) {
  if (LLVM_UNLIKELY(selfHandle->hasIndexedStorage(runtime))) {
    // Note that getStringView can be satisfied without materializing the
    // Identifier.
    const auto strView =
        runtime.getIdentifierTable().getStringView(runtime, name);
    if (auto nameAsIndex = toArrayIndex(strView)) {
      return putComputed_RJS(
          selfHandle,
          runtime,
          runtime.makeHandle(
              HermesValue::encodeTrustedNumberValue(*nameAsIndex)),
          valueHandle,
          opFlags);
    }
    // Here we have indexed properties but the symbol was not index-like.
    // Fall through to putNamed().
  }
  return putNamed_RJS(selfHandle, runtime, name, valueHandle, opFlags);
}

CallResult<bool> JSObject::putComputedWithReceiver_RJS(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<> nameValHandle,
    Handle<> valueHandle,
    Handle<> receiver,
    PropOpFlags opFlags) {
  assert(
      !opFlags.getMustExist() &&
      "mustExist flag cannot be used with computed properties");

  // Try the fast-path first: has "index-like" properties, the "name"
  // already is a valid integer index, selfHandle and receiver are the
  // same, and it is present in storage.
  if (selfHandle->hasFastIndexProperties(runtime)) {
    if (auto arrayIndex = toArrayIndexFastPath(*nameValHandle)) {
      if (selfHandle.getHermesValue().getRaw() == receiver->getRaw()) {
        if (haveOwnIndexed(selfHandle.get(), runtime, *arrayIndex)) {
          auto result =
              setOwnIndexed(selfHandle, runtime, *arrayIndex, valueHandle);
          if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION))
            return ExecutionStatus::EXCEPTION;
          if (LLVM_LIKELY(*result))
            return true;
          if (opFlags.getThrowOnError()) {
            // TODO: better message.
            return runtime.raiseTypeError(
                "Cannot assign to read-only property");
          }
          return false;
        }
      }
    }
  }

  // If nameValHandle is an object, we should convert it to string now,
  // because toString may have side-effect, and we want to do this only
  // once.
  auto converted = toPropertyKeyIfObject(runtime, nameValHandle);
  if (LLVM_UNLIKELY(converted == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto nameValPrimitiveHandle = *converted;

  ComputedPropertyDescWithSymStorage desc{MutableHandle<SymbolID>{runtime}};

  // Look for the property in this object or along the prototype chain.
  MutableHandle<JSObject> propObj{runtime};
  if (LLVM_UNLIKELY(
          getComputedPrimitiveDescriptor(
              selfHandle, runtime, nameValPrimitiveHandle, propObj, desc) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // If the property exists (or, we hit a proxy/hostobject on the way
  // up the chain)
  if (propObj) {
    // Get the simple case out of the way: If the property already
    // exists on selfHandle, is not an accessor, selfHandle and
    // receiver are the same, selfHandle is not a host
    // object/proxy/internal setter, and the property is writable,
    // just write into the same slot.

    if (LLVM_LIKELY(
            selfHandle == propObj &&
            selfHandle.getHermesValue().getRaw() == receiver->getRaw() &&
            !desc.flags.accessor && !desc.flags.internalSetter &&
            !desc.flags.hostObject && !desc.flags.proxyObject &&
            desc.flags.writable)) {
      if (LLVM_UNLIKELY(
              setComputedSlotValueUnsafe(
                  selfHandle, runtime, desc, valueHandle) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      return true;
    }

    // Is it an accessor?
    if (LLVM_UNLIKELY(desc.flags.accessor)) {
      auto *accessor = vmcast<PropertyAccessor>(
          getComputedSlotValueUnsafe(propObj, runtime, desc));

      // If it is a read-only accessor, fail.
      if (!accessor->setter) {
        if (opFlags.getThrowOnError()) {
          return runtime.raiseTypeErrorForValue(
              "Cannot assign to property ",
              nameValPrimitiveHandle,
              " which has only a getter");
        }
        return false;
      }

      // Execute the accessor on this object.
      if (accessor->setter.getNonNull(runtime)->executeCall1(
              runtime.makeHandle(accessor->setter),
              runtime,
              receiver,
              valueHandle.get()) == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      return true;
    }

    if (LLVM_UNLIKELY(desc.flags.proxyObject)) {
      CallResult<Handle<>> key = toPropertyKey(runtime, nameValPrimitiveHandle);
      if (key == ExecutionStatus::EXCEPTION)
        return ExecutionStatus::EXCEPTION;
      CallResult<bool> setRes =
          JSProxy::setComputed(propObj, runtime, *key, valueHandle, receiver);
      if (LLVM_UNLIKELY(setRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (!*setRes && opFlags.getThrowOnError()) {
        // TODO: better message.
        return runtime.raiseTypeError(
            TwineChar16("Proxy trap returned false for property"));
      }
      return setRes;
    }

    if (LLVM_UNLIKELY(!desc.flags.writable)) {
      if (desc.flags.staticBuiltin) {
        SymbolID id{};
        LAZY_TO_IDENTIFIER(runtime, nameValPrimitiveHandle, id);
        return raiseErrorForOverridingStaticBuiltin(
            selfHandle, runtime, runtime.makeHandle(id));
      }
      if (opFlags.getThrowOnError()) {
        return runtime.raiseTypeErrorForValue(
            "Cannot assign to read-only property ", nameValPrimitiveHandle, "");
      }
      return false;
    }

    if (selfHandle == propObj && desc.flags.internalSetter) {
      SymbolID id{};
      LAZY_TO_IDENTIFIER(runtime, nameValPrimitiveHandle, id);
      return internalSetter(
          selfHandle,
          runtime,
          id,
          desc.castToNamedPropertyDescriptorRef(),
          valueHandle,
          opFlags);
    }
  }

  // The property does not exist as an conventional own property on
  // this object.

  MutableHandle<JSObject> receiverHandle{runtime, *selfHandle};
  if (selfHandle.getHermesValue().getRaw() != receiver->getRaw() ||
      receiverHandle->isHostObject(runtime) ||
      receiverHandle->isProxyObject(runtime)) {
    if (selfHandle.getHermesValue().getRaw() != receiver->getRaw()) {
      receiverHandle = dyn_vmcast<JSObject>(*receiver);
    }
    if (!receiverHandle) {
      return false;
    }
    ComputedPropertyDescriptor existingDesc;
    CallResult<bool> descDefinedRes = getOwnComputedPrimitiveDescriptor(
        receiverHandle,
        runtime,
        nameValPrimitiveHandle,
        IgnoreProxy::No,
        existingDesc);
    if (LLVM_UNLIKELY(descDefinedRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    DefinePropertyFlags dpf;
    if (*descDefinedRes) {
      if (LLVM_UNLIKELY(
              existingDesc.flags.accessor || !existingDesc.flags.writable)) {
        return false;
      }

      if (LLVM_LIKELY(
              !existingDesc.flags.internalSetter &&
              !receiverHandle->isHostObject(runtime) &&
              !receiverHandle->isProxyObject(runtime))) {
        if (LLVM_UNLIKELY(
                setComputedSlotValueUnsafe(
                    receiverHandle, runtime, existingDesc, valueHandle) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        return true;
      }
    }

    // At this point, either the descriptor exists on the receiver,
    // but it's a corner case; or, there was no descriptor.
    if (LLVM_UNLIKELY(
            existingDesc.flags.internalSetter ||
            receiverHandle->isHostObject(runtime) ||
            receiverHandle->isProxyObject(runtime))) {
      // If putComputed is called on a proxy whose target's prototype
      // is an array with a propname of 'length', then internalSetter
      // will be true, and the receiver will be a proxy.  In that case,
      // proxy wins.
      if (receiverHandle->isProxyObject(runtime)) {
        if (*descDefinedRes) {
          dpf.setValue = 1;
        } else {
          dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
        }
        return JSProxy::defineOwnProperty(
            receiverHandle,
            runtime,
            nameValPrimitiveHandle,
            dpf,
            valueHandle,
            opFlags);
      }
      SymbolID id{};
      LAZY_TO_IDENTIFIER(runtime, nameValPrimitiveHandle, id);
      if (existingDesc.flags.internalSetter) {
        return internalSetter(
            receiverHandle,
            runtime,
            id,
            existingDesc.castToNamedPropertyDescriptorRef(),
            valueHandle,
            opFlags);
      }
      assert(
          receiverHandle->isHostObject(runtime) &&
          "descriptor flags are impossible");
      return vmcast<HostObject>(receiverHandle.get())->set(id, *valueHandle);
    }
  }

  /// Can we add more properties?
  if (LLVM_UNLIKELY(!receiverHandle->isExtensible(runtime))) {
    if (opFlags.getThrowOnError()) {
      return runtime.raiseTypeError(
          "cannot add a new property"); // TODO: better message.
    }
    return false;
  }

  // If we have indexed storage we must check whether the property is an index,
  // and if it is, store it in indexed storage.
  if (receiverHandle->hasIndexedStorage(runtime)) {
    OptValue<uint32_t> arrayIndex;
    StringPrimitive *strPrim = nullptr;
    TO_ARRAY_INDEX(runtime, nameValPrimitiveHandle, strPrim, arrayIndex);
    if (arrayIndex) {
      // Check whether we need to update array's ".length" property.
      if (auto *array = dyn_vmcast<JSArray>(receiverHandle.get())) {
        if (LLVM_UNLIKELY(*arrayIndex >= JSArray::getLength(array, runtime))) {
          auto cr = putNamed_RJS(
              receiverHandle,
              runtime,
              Predefined::getSymbolID(Predefined::length),
              runtime.makeHandle(
                  HermesValue::encodeTrustedNumberValue(*arrayIndex + 1)),
              opFlags);
          if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
            return ExecutionStatus::EXCEPTION;
          if (LLVM_UNLIKELY(!*cr))
            return false;
        }
      }

      auto result =
          setOwnIndexed(receiverHandle, runtime, *arrayIndex, valueHandle);
      if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      if (LLVM_LIKELY(*result))
        return true;

      if (opFlags.getThrowOnError()) {
        // TODO: better message.
        return runtime.raiseTypeError("Cannot assign to read-only property");
      }
      return false;
    }
  }

  SymbolID id{};
  LAZY_TO_IDENTIFIER(runtime, nameValPrimitiveHandle, id);

  // Add a new named property.
  return addOwnProperty(
      receiverHandle,
      runtime,
      id,
      DefinePropertyFlags::getDefaultNewPropertyFlags(),
      valueHandle,
      opFlags);
}

CallResult<bool> JSObject::deleteNamed(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SymbolID name,
    PropOpFlags opFlags) {
  assert(
      !opFlags.getMustExist() && "mustExist cannot be specified when deleting");

  // Find the property by name.
  NamedPropertyDescriptor desc;
  auto pos = findProperty(selfHandle, runtime, name, desc);

  // If the property doesn't exist in this object, return success.
  if (!pos) {
    if (LLVM_LIKELY(
            !selfHandle->isLazy(runtime) &&
            !selfHandle->isProxyObject(runtime))) {
      return true;
    } else if (selfHandle->isLazy(runtime)) {
      // object is lazy, initialize and read again.
      initializeLazyObject(runtime, selfHandle);
      pos = findProperty(selfHandle, runtime, name, desc);
      if (!pos) // still not there, return true.
        return true;
    } else {
      assert(
          selfHandle->isProxyObject(runtime) && "object flags are impossible");
      return proxyOpFlags(
          runtime,
          opFlags,
          "Proxy delete returned false",
          JSProxy::deleteNamed(selfHandle, runtime, name));
    }
  }
  // If the property isn't configurable, fail.
  if (LLVM_UNLIKELY(!desc.flags.configurable)) {
    if (opFlags.getThrowOnError()) {
      return runtime.raiseTypeError(
          TwineChar16("Property '") +
          runtime.getIdentifierTable().getStringViewForDev(runtime, name) +
          "' is not configurable");
    }
    return false;
  }

  // Clear the deleted property value to prevent memory leaks.
  setNamedSlotValueUnsafe(
      *selfHandle, runtime, desc, SmallHermesValue::encodeEmptyValue());

  // Perform the actual deletion.
  auto newClazz = HiddenClass::deleteProperty(
      runtime.makeHandle(selfHandle->getClassGCPtr()), runtime, *pos);
  selfHandle->updateClass(runtime, *newClazz);

  return true;
}

CallResult<bool> JSObject::deleteComputed(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<> nameValHandle,
    PropOpFlags opFlags) {
  assert(
      !opFlags.getMustExist() && "mustExist cannot be specified when deleting");

  // If nameValHandle is an object, we should convert it to string now,
  // because toString may have side-effect, and we want to do this only
  // once.
  auto converted = toPropertyKeyIfObject(runtime, nameValHandle);
  if (LLVM_UNLIKELY(converted == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  auto nameValPrimitiveHandle = *converted;

  // If the name is a valid integer array index, store it here.
  OptValue<uint32_t> arrayIndex;

  // If we have indexed storage, we must attempt to convert the name to array
  // index, even if the conversion is expensive.
  if (selfHandle->hasIndexedStorage(runtime)) {
    StringPrimitive *strPrim = nullptr;
    TO_ARRAY_INDEX(runtime, nameValPrimitiveHandle, strPrim, arrayIndex);
  }

  // Try the fast-path first: the "name" is a valid array index and we don't
  // have "index-like" named properties.
  if (arrayIndex && selfHandle->hasFastIndexProperties(runtime)) {
    // Delete the indexed property.
    if (deleteOwnIndexed(selfHandle, runtime, *arrayIndex))
      return true;

    // Cannot delete property (for example this may be a typed array).
    if (opFlags.getThrowOnError()) {
      // TODO: better error message.
      return runtime.raiseTypeError("Cannot delete property");
    }
    return false;
  }

  // slow path, check if object is lazy before continuing.
  if (LLVM_UNLIKELY(selfHandle->isLazy(runtime))) {
    // initialize and try again.
    initializeLazyObject(runtime, selfHandle);
    return deleteComputed(selfHandle, runtime, nameValHandle, opFlags);
  }

  // Convert the string to an SymbolID;
  SymbolID id;
  LAZY_TO_IDENTIFIER(runtime, nameValPrimitiveHandle, id);

  // Find the property by name.
  NamedPropertyDescriptor desc;
  auto pos = findProperty(selfHandle, runtime, id, desc);

  // If the property exists, make sure it is configurable.
  if (pos) {
    // If the property isn't configurable, fail.
    if (LLVM_UNLIKELY(!desc.flags.configurable)) {
      if (opFlags.getThrowOnError()) {
        // TODO: a better message.
        return runtime.raiseTypeError("Property is not configurable");
      }
      return false;
    }
  }

  // At this point we know that the named property either doesn't exist, or
  // is configurable and so can be deleted, or the object is a Proxy.

  // If it is an "index-like" property, we must also delete the "shadow" indexed
  // property in order to keep Array.length correct.
  if (arrayIndex) {
    if (!deleteOwnIndexed(selfHandle, runtime, *arrayIndex)) {
      // Cannot delete property (for example this may be a typed array).
      if (opFlags.getThrowOnError()) {
        // TODO: better error message.
        return runtime.raiseTypeError("Cannot delete property");
      }
      return false;
    }
  }

  if (pos) {
    // delete the named property (if it exists).
    // Clear the deleted property value to prevent memory leaks.
    setNamedSlotValueUnsafe(
        *selfHandle, runtime, desc, SmallHermesValue::encodeEmptyValue());

    // Remove the property descriptor.
    auto newClazz = HiddenClass::deleteProperty(
        runtime.makeHandle(selfHandle->getClassGCPtr()), runtime, *pos);
    selfHandle->updateClass(runtime, *newClazz);
  } else if (LLVM_UNLIKELY(selfHandle->isProxyObject(runtime))) {
    CallResult<Handle<>> key = toPropertyKey(runtime, nameValPrimitiveHandle);
    if (key == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    return proxyOpFlags(
        runtime,
        opFlags,
        "Proxy delete returned false",
        JSProxy::deleteComputed(selfHandle, runtime, *key));
  }

  return true;
}

CallResult<bool> JSObject::defineOwnPropertyInternal(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SymbolID name,
    DefinePropertyFlags dpFlags,
    Handle<> valueOrAccessor,
    PropOpFlags opFlags) {
  assert(
      !opFlags.getMustExist() && "cannot use mustExist with defineOwnProperty");
  assert(
      !(dpFlags.setValue && dpFlags.isAccessor()) &&
      "Cannot set both value and accessor");
  assert(
      (dpFlags.setValue || dpFlags.isAccessor() ||
       valueOrAccessor.get().isUndefined()) &&
      "value must be undefined when all of setValue/setSetter/setGetter are "
      "false");
#ifndef NDEBUG
  if (dpFlags.isAccessor()) {
    assert(valueOrAccessor.get().isPointer() && "accessor must be non-empty");
    assert(
        !dpFlags.setWritable && !dpFlags.writable &&
        "writable must not be set with accessors");
  }
#endif

  // Is it an existing property.
  NamedPropertyDescriptor desc;
  auto pos = findProperty(selfHandle, runtime, name, desc);
  if (pos) {
    return updateOwnProperty(
        selfHandle,
        runtime,
        name,
        *pos,
        desc,
        dpFlags,
        valueOrAccessor,
        opFlags);
  }

  if (LLVM_UNLIKELY(
          selfHandle->isLazy(runtime) || selfHandle->isProxyObject(runtime))) {
    if (selfHandle->isProxyObject(runtime)) {
      return JSProxy::defineOwnProperty(
          selfHandle,
          runtime,
          name.isUniqued() ? runtime.makeHandle(
                                 HermesValue::encodeStringValue(
                                     runtime.getStringPrimFromSymbolID(name)))
                           : runtime.makeHandle(name),
          dpFlags,
          valueOrAccessor,
          opFlags);
    }
    assert(selfHandle->isLazy(runtime) && "descriptor flags are impossible");
    // if the property was not found and the object is lazy we need to
    // initialize it and try again.
    JSObject::initializeLazyObject(runtime, selfHandle);
    return defineOwnPropertyInternal(
        selfHandle, runtime, name, dpFlags, valueOrAccessor, opFlags);
  }

  return addOwnProperty(
      selfHandle, runtime, name, dpFlags, valueOrAccessor, opFlags);
}

ExecutionStatus JSObject::defineNewOwnProperty(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SymbolID name,
    PropertyFlags propertyFlags,
    Handle<> valueOrAccessor) {
  assert(
      (!selfHandle->isProxyObject(runtime) || propertyFlags.privateName) &&
      "cannot define non-private properties on Proxy");
  assert(
      !(propertyFlags.accessor && !valueOrAccessor.get().isPointer()) &&
      "accessor must be non-empty");
  assert(
      !(propertyFlags.accessor && propertyFlags.writable) &&
      "writable must not be set with accessors");
  assert(
      !HiddenClass::debugIsPropertyDefined(
          selfHandle->getClass(runtime), runtime, name) &&
      "new property is already defined");
  // Ensure the object is non-lazy before adding the property.
  if (selfHandle->isLazy(runtime))
    initializeLazyObject(runtime, selfHandle);

  return addOwnPropertyImpl(
      selfHandle,
      runtime,
      name,
      propertyFlags,
      valueOrAccessor,
      /* runtimeModule */ nullptr,
      /* unit */ nullptr,
      /* cacheEntry */ nullptr);
}

CallResult<bool> JSObject::defineOwnComputedPrimitive(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<> nameValHandle,
    DefinePropertyFlags dpFlags,
    Handle<> valueOrAccessor,
    PropOpFlags opFlags) {
  assert(
      !nameValHandle->isObject() &&
      "nameValHandle passed to "
      "defineOwnComputedPrimitive() cannot be "
      "an object");
  assert(
      !opFlags.getMustExist() && "cannot use mustExist with defineOwnProperty");
  assert(
      !(dpFlags.setValue && dpFlags.isAccessor()) &&
      "Cannot set both value and accessor");
  assert(
      (dpFlags.setValue || dpFlags.isAccessor() ||
       valueOrAccessor.get().isUndefined()) &&
      "value must be undefined when all of setValue/setSetter/setGetter are "
      "false");
  assert(
      !dpFlags.enableInternalSetter &&
      "Cannot set internalSetter on a computed property");
#ifndef NDEBUG
  if (dpFlags.isAccessor()) {
    assert(valueOrAccessor.get().isPointer() && "accessor must be non-empty");
    assert(
        !dpFlags.setWritable && !dpFlags.writable &&
        "writable must not be set with accessors");
  }
#endif

  // If the name is a valid integer array index, store it here.
  OptValue<uint32_t> arrayIndex;

  // If we have indexed storage, we must attempt to convert the name to array
  // index, even if the conversion is expensive.
  if (selfHandle->hasIndexedStorage(runtime)) {
    StringPrimitive *strPrim = nullptr;
    TO_ARRAY_INDEX(runtime, nameValHandle, strPrim, arrayIndex);
  }

  SymbolID id{};

  // If not storing a property with an array index name, or if we don't have
  // indexed storage, just pass to the named routine.
  if (!arrayIndex) {
    // TODO(T125334872): properly handle the case when self is a TypedArray.
    LAZY_TO_IDENTIFIER(runtime, nameValHandle, id);
    return defineOwnPropertyInternal(
        selfHandle, runtime, id, dpFlags, valueOrAccessor, opFlags);
  }

  // At this point we know that we have indexed storage and that the property
  // has an index-like name.

  // First check if a named property with the same name exists.
  if (selfHandle->getClass(runtime)->getHasIndexLikeProperties()) {
    LAZY_TO_IDENTIFIER(runtime, nameValHandle, id);

    NamedPropertyDescriptor desc;
    auto pos = findProperty(selfHandle, runtime, id, desc);
    // If we found a named property, update it.
    if (pos) {
      return updateOwnProperty(
          selfHandle,
          runtime,
          id,
          *pos,
          desc,
          dpFlags,
          valueOrAccessor,
          opFlags);
    }
  }

  // Does an indexed property with that index exist?
  auto indexedPropPresent =
      getOwnIndexedPropertyFlags(selfHandle.get(), runtime, *arrayIndex);
  if (indexedPropPresent) {
    // The current value of the property.
    Handle<> curValueOrAccessor =
        runtime.makeHandle(getOwnIndexed(selfHandle, runtime, *arrayIndex));

    auto updateStatus = checkPropertyUpdate(
        runtime,
        *indexedPropPresent,
        dpFlags,
        *curValueOrAccessor,
        valueOrAccessor,
        opFlags);
    if (updateStatus == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    if (updateStatus->first == PropertyUpdateStatus::failed)
      return false;

    // The property update is valid, but can the property remain an "indexed"
    // property, or do we need to convert it to a named property?
    // If the property flags didn't change, the property remains indexed.
    if (updateStatus->second == *indexedPropPresent) {
      // If the value doesn't change, we are done.
      if (updateStatus->first == PropertyUpdateStatus::done)
        return true;

      // If we successfully updated the value, we are done.
      auto result =
          setOwnIndexed(selfHandle, runtime, *arrayIndex, valueOrAccessor);
      if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      if (*result)
        return true;

      if (opFlags.getThrowOnError()) {
        // TODO: better error message.
        return runtime.raiseTypeError("cannot change read-only property value");
      }

      return false;
    }

    // OK, we need to convert an indexed property to a named one.

    // Check whether to use the supplied value, or to reuse the old one, as we
    // are simply reconfiguring it.
    MutableHandle<> value{runtime};
    if (dpFlags.setValue || dpFlags.isAccessor()) {
      value = valueOrAccessor.get();
    } else {
      value = *curValueOrAccessor;
    }

    // Delete the existing indexed property.
    if (!deleteOwnIndexed(selfHandle, runtime, *arrayIndex)) {
      if (opFlags.getThrowOnError()) {
        // TODO: better error message.
        return runtime.raiseTypeError("Cannot define property");
      }
      return false;
    }

    // Add the new named property. Call addOwnPropertyImpl directly, since the
    // property must be added.
    LAZY_TO_IDENTIFIER(runtime, nameValHandle, id);
    if (LLVM_UNLIKELY(
            addOwnPropertyImpl(
                selfHandle,
                runtime,
                id,
                updateStatus->second,
                value,
                /* runtimeModule */ nullptr,
                /* unit */ nullptr,
                /* cacheEntry */ nullptr) == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    return true;
  }

  /// Can we add new properties?
  if (!selfHandle->isExtensible(runtime)) {
    if (opFlags.getThrowOnError()) {
      return runtime.raiseTypeError(
          "cannot add a new property"); // TODO: better message.
    }
    return false;
  }

  // This is a new property with an index-like name.
  // Check whether we need to update array's ".length" property.
  bool updateLength = false;
  if (auto arrayHandle = Handle<JSArray>::dyn_vmcast(selfHandle)) {
    if (LLVM_UNLIKELY(
            *arrayIndex >= JSArray::getLength(*arrayHandle, runtime))) {
      NamedPropertyDescriptor lengthDesc;
      bool lengthPresent = getOwnNamedDescriptor(
          arrayHandle,
          runtime,
          Predefined::getSymbolID(Predefined::length),
          lengthDesc);
      (void)lengthPresent;
      assert(lengthPresent && ".length must be present in JSArray");

      if (!lengthDesc.flags.writable) {
        if (opFlags.getThrowOnError()) {
          return runtime.raiseTypeError(
              "Cannot assign to read-only 'length' property of array");
        }
        return false;
      }

      updateLength = true;
    }
  }

  bool newIsIndexed = canNewPropertyBeIndexed(dpFlags);
  if (newIsIndexed) {
    auto result = setOwnIndexed(
        selfHandle,
        runtime,
        *arrayIndex,
        dpFlags.setValue ? valueOrAccessor : Runtime::getUndefinedValue());
    if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    if (!*result) {
      if (opFlags.getThrowOnError()) {
        // TODO: better error message.
        return runtime.raiseTypeError("Cannot define property");
      }
      return false;
    }
  }

  // If this is an array and we need to update ".length", do so.
  if (updateLength) {
    // This should always succeed since we are simply enlarging the length.
    auto res = JSArray::setLength(
        Handle<JSArray>::vmcast(selfHandle), runtime, *arrayIndex + 1, opFlags);
    (void)res;
    assert(
        res != ExecutionStatus::EXCEPTION && *res &&
        "JSArray::setLength() failed unexpectedly");
  }

  if (newIsIndexed)
    return true;

  // We are adding a new property with an index-like name.
  // TODO(T125334872): properly handle the case when self is a TypedArray.
  LAZY_TO_IDENTIFIER(runtime, nameValHandle, id);
  return addOwnProperty(
      selfHandle, runtime, id, dpFlags, valueOrAccessor, opFlags);
}

CallResult<bool> JSObject::defineOwnComputed(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<> nameValHandle,
    DefinePropertyFlags dpFlags,
    Handle<> valueOrAccessor,
    PropOpFlags opFlags) {
  auto converted = toPropertyKeyIfObject(runtime, nameValHandle);
  if (LLVM_UNLIKELY(converted == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return defineOwnComputedPrimitive(
      selfHandle, runtime, *converted, dpFlags, valueOrAccessor, opFlags);
}

CallResult<HermesValue> JSObject::getPrivateField(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<SymbolID> privateName,
    PrivateNameCacheEntry *cacheEntry) {
  NamedPropertyDescriptor desc;
  // 1. Let entry be PrivateElementFind(O, P).
  auto pos = findProperty(selfHandle, runtime, *privateName, desc);
  // 2. If entry is empty, throw a TypeError exception.
  if (!pos.hasValue()) {
    return runtime.raiseTypeErrorForValue(
        privateName, "Private property does not exist: ");
  }
  // This must be a private field since methods and accessors are not stored
  // directly in the instance. So just directly return it.
  if (cacheEntry && !selfHandle->getClass(runtime)->isDictionaryNoCache()) {
    cacheEntry->clazz = selfHandle->getClassGCPtr();
    cacheEntry->slot = desc.slot;
    cacheEntry->nameVal = *privateName;
  }
  return getNamedSlotValueUnsafe(*selfHandle, runtime, desc).unboxToHV(runtime);
}

ExecutionStatus JSObject::setPrivateField(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<SymbolID> privateName,
    Handle<> valueHandle,
    PrivateNameCacheEntry *cacheEntry) {
  NamedPropertyDescriptor desc;
  // 1. Let entry be PrivateElementFind(O, P).
  auto pos = findProperty(selfHandle, runtime, *privateName, desc);
  // 2. If entry is empty, throw a TypeError exception.
  if (!pos.hasValue()) {
    return runtime.raiseTypeErrorForValue(
        privateName, "Private property does not exist: ");
  }
  // This must be a private field since methods and accessors are not stored
  // directly in the instance. So just directly set it.
  if (cacheEntry && !selfHandle->getClass(runtime)->isDictionaryNoCache()) {
    cacheEntry->clazz = selfHandle->getClassGCPtr();
    cacheEntry->slot = desc.slot;
    cacheEntry->nameVal = static_cast<RootSymbolID>(*privateName);
  }
  auto shv = SmallHermesValue::encodeHermesValue(*valueHandle, runtime);
  setNamedSlotValueUnsafe(*selfHandle, runtime, desc, shv);
  return ExecutionStatus::RETURNED;
}

std::string JSObject::getNameIfExists(PointerBase &base) {
  // Try "displayName" first, if it is defined.
  if (auto nameVal = tryGetNamedNoAlloc(
          this, base, Predefined::getSymbolID(Predefined::displayName))) {
    if (auto *name = dyn_vmcast<StringPrimitive>(nameVal->unboxToHV(base))) {
      return converter(name);
    }
  }
  // Next, use "name" if it is defined.
  if (auto nameVal = tryGetNamedNoAlloc(
          this, base, Predefined::getSymbolID(Predefined::name))) {
    if (auto *name = dyn_vmcast<StringPrimitive>(nameVal->unboxToHV(base))) {
      return converter(name);
    }
  }
  // There is no other way to access the "name" property on an object.
  return "";
}

#ifdef HERMES_MEMORY_INSTRUMENTATION
std::string JSObject::getHeuristicTypeName(GC &gc) {
  PointerBase &base = gc.getPointerBase();
  if (auto constructorVal = tryGetNamedNoAlloc(
          this, base, Predefined::getSymbolID(Predefined::constructor))) {
    if (auto *constructor = dyn_vmcast<JSObject>(
            constructorVal->unboxToHV(gc.getPointerBase()))) {
      auto name = constructor->getNameIfExists(base);
      // If the constructor's name doesn't exist, or it is just the object
      // constructor, attempt to find a different name.
      if (!name.empty() && name != "Object")
        return name;
    }
  }

  std::string name = getVT()->snapshotMetaData.defaultNameForNode(this);
  // A constructor's name was not found, check if the object is in dictionary
  // mode.
  if (getClass(base)->isDictionary()) {
    return name + "(Dictionary)";
  }

  // If it's not an Object, the CellKind is most likely good enough on its own
  if (getKind() != CellKind::JSObjectKind) {
    return name;
  }

  // If the object isn't a dictionary, and it has only a few property names,
  // make the name based on those property names.
  std::vector<std::string> propertyNames;
  HiddenClass::forEachPropertyNoAlloc(
      getClass(base),
      base,
      [&gc, &propertyNames](SymbolID id, NamedPropertyDescriptor) {
        if (InternalProperty::isInternal(id)) {
          // Internal properties aren't user-visible, skip them.
          return;
        }
        propertyNames.emplace_back(gc.convertSymbolToUTF8(id));
      });
  // NOTE: One option is to sort the property names before truncation, to
  // reduce the number of groups; however, by not sorting them it makes it
  // easier to spot sets of objects with the same properties but in different
  // orders, and thus find HiddenClass optimizations to make.

  // For objects with a lot of properties but aren't in dictionary mode yet,
  // keep the number displayed small.
  constexpr int kMaxPropertiesForTypeName = 5;
  bool truncated = false;
  if (propertyNames.size() > kMaxPropertiesForTypeName) {
    propertyNames.erase(
        propertyNames.begin() + kMaxPropertiesForTypeName, propertyNames.end());
    truncated = true;
  }
  // The final name should look like Object(a, b, c).
  if (propertyNames.empty()) {
    // Don't add parentheses for objects with no properties.
    return name;
  }
  name += "(";
  bool first = true;
  for (const auto &prop : propertyNames) {
    if (!first) {
      name += ", ";
    }
    first = false;
    name += prop;
  }
  if (truncated) {
    // No need to check for comma edge case because this only happens for
    // greater than one property.
    static_assert(
        kMaxPropertiesForTypeName >= 1,
        "Property truncation should not happen for 0 properties");
    name += ", ...";
  }
  name += ")";
  return name;
}

std::string JSObject::_snapshotNameImpl(GCCell *cell, GC &gc) {
  auto *const self = vmcast<JSObject>(cell);
  return self->getHeuristicTypeName(gc);
}

void JSObject::_snapshotAddEdgesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap) {
  auto *const self = vmcast<JSObject>(cell);

  // Add the prototype as a property edge, so it's easy for JS developers to
  // walk the prototype chain on their own.
  if (self->getParentGCPtr(gc.getPointerBase())) {
    snap.addNamedEdge(
        HeapSnapshot::EdgeType::Property,
        // __proto__ chosen for similarity to V8.
        "__proto__",
        gc.getObjectID(self->getParent(gc.getPointerBase())));
  }

  HiddenClass::forEachPropertyNoAlloc(
      self->getClass(gc.getPointerBase()),
      gc.getPointerBase(),
      [self, &gc, &snap](SymbolID id, NamedPropertyDescriptor desc) {
        if (InternalProperty::isInternal(id)) {
          // Internal properties aren't user-visible, skip them.
          return;
        }
        // Else, it's a user-visible property.
        HermesValue prop =
            getNamedSlotValueUnsafe(self, gc.getPointerBase(), desc.slot)
                .toHV(gc.getPointerBase());
        const llvh::Optional<HeapSnapshot::NodeID> idForProp =
            gc.getSnapshotID(prop);
        if (!idForProp) {
          return;
        }
        std::string propName = gc.convertSymbolToUTF8(id);
        // If the property name is a valid array index, display it as an
        // "element" instead of a "property". This will put square brackets
        // around the number and sort it numerically rather than
        // alphabetically.
        if (auto index = ::hermes::toArrayIndex(propName)) {
          snap.addIndexedEdge(
              HeapSnapshot::EdgeType::Element,
              index.getValue(),
              idForProp.getValue());
        } else {
          snap.addNamedEdge(
              HeapSnapshot::EdgeType::Property, propName, idForProp.getValue());
        }
      });
}

void JSObject::_snapshotAddLocationsImpl(
    GCCell *cell,
    GC &gc,
    HeapSnapshot &snap) {
  auto *const self = vmcast<JSObject>(cell);
  PointerBase &base = gc.getPointerBase();
  // Add the location of the constructor function for this object, if that
  // constructor is a user-defined JS function.
  if (auto constructorVal = tryGetNamedNoAlloc(
          self, base, Predefined::getSymbolID(Predefined::constructor))) {
    if (constructorVal->isObject()) {
      if (auto *constructor =
              dyn_vmcast<JSFunction>(constructorVal->getObject(base))) {
        constructor->addLocationToSnapshot(snap, gc.getObjectID(self), gc);
      }
    }
  }
}
#endif

std::pair<uint32_t, uint32_t> JSObject::_getOwnIndexedRangeImpl(
    JSObject *self,
    PointerBase &) {
  return {0, 0};
}

bool JSObject::_haveOwnIndexedImpl(JSObject *self, Runtime &, uint32_t) {
  return false;
}

OptValue<PropertyFlags> JSObject::_getOwnIndexedPropertyFlagsImpl(
    JSObject *self,
    Runtime &runtime,
    uint32_t) {
  return llvh::None;
}

HermesValue
JSObject::_getOwnIndexedImpl(PseudoHandle<JSObject>, Runtime &, uint32_t) {
  return HermesValue::encodeEmptyValue();
}

CallResult<bool>
JSObject::_setOwnIndexedImpl(Handle<JSObject>, Runtime &, uint32_t, Handle<>) {
  return false;
}

bool JSObject::_deleteOwnIndexedImpl(Handle<JSObject>, Runtime &, uint32_t) {
  return false;
}

bool JSObject::_checkAllOwnIndexedImpl(
    JSObject * /*self*/,
    Runtime & /*runtime*/,
    ObjectVTable::CheckAllOwnIndexedMode /*mode*/) {
  return true;
}

HermesValue JSObject::_jitCallImpl(Runtime *runtime, JSObject *self) {
  {
    struct : Locals {
      PinnedValue<JSObject> target;
    } lv;
    LocalsRAII lraii{*runtime, &lv};
    lv.target = self;
    // Throw an exception because this object is not callable.
    (void)runtime->raiseTypeErrorForValue(lv.target, " is not a function");
  }
  _sh_throw_current(runtime);
}

void JSObject::preventExtensionsNonProxy(
    Handle<JSObject> self,
    Runtime &runtime) {
  assert(
      !self->isProxyObject(runtime) &&
      "[[Extensible]] slot cannot be set directly on Proxy objects");
  // Lazy objects must be materialized before their class flags change,
  // because materialization replaces the class.
  if (LLVM_UNLIKELY(self->isLazy(runtime)))
    initializeLazyObject(runtime, self);
  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  lv.clazz = self->getClass(runtime);
  HiddenClass *newClazz = HiddenClass::markNoExtend(lv.clazz, runtime);
  self->updateClass(runtime, newClazz);
}

CallResult<bool> JSObject::preventExtensions(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    PropOpFlags opFlags) {
  if (LLVM_UNLIKELY(selfHandle->isProxyObject(runtime))) {
    return JSProxy::preventExtensions(selfHandle, runtime, opFlags);
  }
  JSObject::preventExtensionsNonProxy(selfHandle, runtime);
  return true;
}

ExecutionStatus JSObject::seal(Handle<JSObject> selfHandle, Runtime &runtime) {
  if (LLVM_UNLIKELY(selfHandle->isProxyObject(runtime))) {
    CallResult<bool> statusRes = JSProxy::preventExtensions(
        selfHandle, runtime, PropOpFlags().plusThrowOnError());
    if (LLVM_UNLIKELY(statusRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    assert(
        *statusRes &&
        "seal preventExtensions with ThrowOnError returned false");
  }

  if (LLVM_UNLIKELY(selfHandle->isLazy(runtime)))
    initializeLazyObject(runtime, selfHandle);

  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.clazz = selfHandle->getClass(runtime);

  // Already sealed?
  if (lv.clazz->isSealed())
    return ExecutionStatus::RETURNED;

  lv.clazz = HiddenClass::seal(lv.clazz, runtime);
  selfHandle->updateClass(runtime, *lv.clazz);

  return ExecutionStatus::RETURNED;
}

ExecutionStatus JSObject::freeze(
    Handle<JSObject> selfHandle,
    Runtime &runtime) {
  if (LLVM_UNLIKELY(selfHandle->isProxyObject(runtime))) {
    CallResult<bool> statusRes = JSProxy::preventExtensions(
        selfHandle, runtime, PropOpFlags().plusThrowOnError());
    if (LLVM_UNLIKELY(statusRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    assert(*statusRes && "preventExtensions with ThrowOnError returned false");
  }

  if (LLVM_UNLIKELY(selfHandle->isLazy(runtime)))
    initializeLazyObject(runtime, selfHandle);

  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.clazz = selfHandle->getClass(runtime);

  // Already frozen?
  if (lv.clazz->isFrozen())
    return ExecutionStatus::RETURNED;

  lv.clazz = HiddenClass::freeze(lv.clazz, runtime);
  selfHandle->updateClass(runtime, *lv.clazz);

  return ExecutionStatus::RETURNED;
}

void JSObject::updatePropertyFlagsWithoutTransitions(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    PropertyFlags flagsToClear,
    PropertyFlags flagsToSet,
    OptValue<llvh::ArrayRef<SymbolID>> props) {
  auto newClazz = HiddenClass::updatePropertyFlagsWithoutTransitions(
      runtime.makeHandle(selfHandle->getClassGCPtr()),
      runtime,
      flagsToClear,
      flagsToSet,
      props);
  selfHandle->updateClass(runtime, *newClazz);
}

CallResult<bool> JSObject::isExtensible(
    PseudoHandle<JSObject> self,
    Runtime &runtime) {
  if (LLVM_UNLIKELY(self->isProxyObject(runtime))) {
    return JSProxy::isExtensible(runtime.makeHandle(std::move(self)), runtime);
  }
  return self->isExtensible(runtime);
}

bool JSObject::isSealed(PseudoHandle<JSObject> self, Runtime &runtime) {
  if (self->getClass(runtime)->isSealed())
    return true;
  if (self->getClass(runtime)->isExtensible())
    return false;

  struct : public Locals {
    PinnedValue<JSObject> self;
    PinnedValue<HiddenClass> clazz;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.self = std::move(self);
  lv.clazz = lv.self->getClass(runtime);

  if (!HiddenClass::areAllNonConfigurable(lv.clazz, runtime)) {
    return false;
  }

  if (!checkAllOwnIndexed(
          *lv.self,
          runtime,
          ObjectVTable::CheckAllOwnIndexedMode::NonConfigurable)) {
    return false;
  }

  lv.clazz = HiddenClass::markAsSealed(lv.clazz, runtime);
  lv.self->updateClass(runtime, *lv.clazz);

  return true;
}

bool JSObject::isFrozen(PseudoHandle<JSObject> self, Runtime &runtime) {
  if (self->getClass(runtime)->isFrozen())
    return true;
  if (self->getClass(runtime)->isExtensible())
    return false;

  struct : public Locals {
    PinnedValue<JSObject> self;
    PinnedValue<HiddenClass> clazz;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.self = std::move(self);
  lv.clazz = lv.self->getClass(runtime);

  if (!HiddenClass::areAllReadOnly(lv.clazz, runtime)) {
    return false;
  }

  if (!checkAllOwnIndexed(
          *lv.self, runtime, ObjectVTable::CheckAllOwnIndexedMode::ReadOnly)) {
    return false;
  }

  lv.clazz = HiddenClass::markAsFrozen(lv.clazz, runtime);
  lv.self->updateClass(runtime, *lv.clazz);

  return true;
}

void JSObject::setCachedUsingEpoch(Handle<JSObject> self, Runtime &runtime) {
  // Typed objects never change class or parent, so they never need to bump the
  // epoch, and their shared class cannot change flags.
  if (self->getClass(runtime)->isTyped())
    return;
  // Lazy objects must be materialized before their class flags change,
  // because materialization replaces the class.
  if (LLVM_UNLIKELY(self->isLazy(runtime)))
    initializeLazyObject(runtime, self);
  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.clazz = self->getClass(runtime);
  HiddenClass *newClazz =
      HiddenClass::markAsCachedUsingEpoch(lv.clazz, runtime);
  // Ensure that the epoch doesn't get incremented during this operation
  // We're just flipping the flag on the HiddenClass, so it's safe to bypass the
  // increment in this case.
  self->updateClassIgnoringEpochUnsafe(runtime, newClazz);
}

CallResult<bool> JSObject::addOwnProperty(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SymbolID name,
    DefinePropertyFlags dpFlags,
    Handle<> valueOrAccessor,
    PropOpFlags opFlags,
    RuntimeModule *runtimeModule,
    SHUnit *unit,
    WritePropertyCacheEntry *cacheEntry) {
  /// Can we add more properties?
  if (!selfHandle->isExtensible(runtime) && !opFlags.getInternalForce()) {
    if (opFlags.getThrowOnError()) {
      return runtime.raiseTypeError(
          TwineChar16("Cannot add new property '") +
          runtime.getIdentifierTable().getStringViewForDev(runtime, name) +
          "'");
    }
    return false;
  }

  PropertyFlags flags{};

  // Accessors don't set writeable.
  if (dpFlags.isAccessor()) {
    dpFlags.setWritable = 0;
    flags.accessor = 1;
  }

  // Override the default flags if specified.
  if (dpFlags.setEnumerable)
    flags.enumerable = dpFlags.enumerable;
  if (dpFlags.setWritable)
    flags.writable = dpFlags.writable;
  if (dpFlags.setConfigurable)
    flags.configurable = dpFlags.configurable;
  flags.internalSetter = dpFlags.enableInternalSetter;

  if (LLVM_UNLIKELY(
          addOwnPropertyImpl(
              selfHandle,
              runtime,
              name,
              flags,
              valueOrAccessor,
              runtimeModule,
              unit,
              cacheEntry) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return true;
}

void JSObject::tryCacheAddProperty(
    Handle<JSObject> self,
    Runtime &runtime,
    RuntimeModule *runtimeModule,
    SHUnit *unit,
    WritePropertyCacheEntry *writeCacheEntry,
    Handle<HiddenClass> startClazz,
    SlotIndex slot,
    Handle<HiddenClass> resultClazz) {
  assert(writeCacheEntry && !resultClazz->isDictionary());
  assert(*startClazz != *resultClazz && "must be a transition");
  assert(
      ((unit != nullptr) ^ (runtimeModule != nullptr)) &&
      "unable to determine where to allocate AddPropertyCacheEntry");

  // We know the slot fits because the result HC isn't a dictionary.
  static_assert(
      HiddenClass::kDictionaryThreshold < WritePropertyCacheEntry::kMaxSlot);
  assert(slot <= WritePropertyCacheEntry::kMaxSlot);

  {
    NoAllocScope noAlloc{runtime};

    // If any parents are dictionaries, they won't have their HiddenClass change
    // when properties are modified. Parent cache epoch is incremented when the
    // HiddenClass changes, so we can't cache this add.
    // Proxies and HostObjects are excluded for the same reason: what they hold
    // is not described by their HiddenClass, so the epoch cannot track them. A
    // Proxy also cannot be traversed with getParent() at all, since its
    // prototype is given by its target and traps.
    // Perform these checks up here to avoid allocating an unused add cache
    // entry if they fail.
    for (JSObject *cur = self->getParent(runtime); cur != nullptr;
         cur = cur->getParent(runtime)) {
      if (LLVM_UNLIKELY(
              cur->isProxyObject(runtime) || cur->isHostObject(runtime)))
        return;
      if (cur->getClass(runtime)->isDictionary())
        return;
    }

    uint32_t parentCacheEpoch = runtime.getParentCacheEpoch();
    uint32_t addCacheIndex = writeCacheEntry->getAddCacheIndex();
    if (addCacheIndex == 0) {
      // Need to allocate a new AddPropertyCacheEntry in the RuntimeModule.
      if (auto optIdx = runtimeModule
              ? runtimeModule->allocateAddCacheEntry()
              : sh_unit_allocate_add_cache_entry(unit)) {
        addCacheIndex = *optIdx;
        writeCacheEntry->setAddCacheIndex(addCacheIndex);
      } else {
        // Might fail if we're out of bits to use for the index.
        return;
      }
    }

    assert(
        writeCacheEntry->getAddCacheIndex() == addCacheIndex &&
        addCacheIndex != 0);

    // Populate AddPropertyCacheEntry.
    AddPropertyCacheEntry &addCacheEntry = runtimeModule
        ? runtimeModule->getAddCacheEntry(addCacheIndex)
        : sh_unit_get_add_cache_entry(unit, addCacheIndex);
    addCacheEntry.setParentEpochAndSlot(parentCacheEpoch, slot);
    addCacheEntry.startClazz =
        CompressedPointer::encodeNonNull(*startClazz, runtime);
    addCacheEntry.resultClazz =
        CompressedPointer::encodeNonNull(*resultClazz, runtime);
    addCacheEntry.parent = self->getParentGCPtr(runtime);
  }

  struct : public Locals {
    PinnedValue<JSObject> cur;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // Mark everything along the prototype chain as a cached parent.
  for (lv.cur = self->getParent(runtime); lv.cur.get() != nullptr;
       lv.cur = lv.cur->getParent(runtime)) {
    JSObject::setCachedUsingEpoch(lv.cur, runtime);
  }
}

ExecutionStatus JSObject::addOwnPropertyImpl(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SymbolID name,
    PropertyFlags propertyFlags,
    Handle<> valueOrAccessor,
    RuntimeModule *runtimeModule,
    SHUnit *unit,
    WritePropertyCacheEntry *cacheEntry) {
  assert(
      (propertyFlags.privateName || !selfHandle->isProxyObject(runtime)) &&
      "Internal non-private properties cannot be added to Proxy objects");

  struct : public Locals {
    PinnedValue<HiddenClass> startClazz;
    PinnedValue<HiddenClass> resultClazz;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // Add a new property to the class.
  // TODO: if we check for OOM here in the future, we must undo the slot
  // allocation.
  lv.startClazz = selfHandle->getClass(runtime);
  auto addResult =
      HiddenClass::addProperty(lv.startClazz, runtime, name, propertyFlags);
  if (LLVM_UNLIKELY(addResult == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.resultClazz = addResult->first;
  SlotIndex slot = addResult->second;
  selfHandle->updateClass(runtime, *lv.resultClazz);

  if (LLVM_UNLIKELY(
          allocateNewSlotStorage(
              selfHandle, runtime, addResult->second, valueOrAccessor) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (cacheEntry && !lv.resultClazz->isDictionary()) {
    tryCacheAddProperty(
        selfHandle,
        runtime,
        runtimeModule,
        unit,
        cacheEntry,
        lv.startClazz,
        slot,
        lv.resultClazz);
  }

  return ExecutionStatus::RETURNED;
}

void JSObject::addNewOwnPropertyInSlot(
    JSObject *self,
    Runtime &runtime,
    HiddenClass *clazz,
    SlotIndex slot,
    SmallHermesValue value) {
  assert(self && clazz && "must have a HiddenClass to transition to");
  self->updateClass(runtime, clazz);

  if (slot < JSObject::DIRECT_PROPERTY_SLOTS) {
    NoAllocScope noAlloc{runtime};
    JSObject::setNamedSlotValueDirectUnsafe(self, runtime, slot, value);
    return;
  }

  uint32_t indirectSlot = slot - JSObject::DIRECT_PROPERTY_SLOTS;

  if (LLVM_LIKELY(self->propStorage_)) {
    NoAllocScope noAlloc{runtime};
    PropStorage *storage = self->propStorage_.getNonNull(runtime);
    assert(
        indirectSlot == storage->size() && "must be adding the next property");
    if (LLVM_LIKELY(indirectSlot < storage->capacity())) {
      storage->pushWithinCapacity(runtime, value);
      return;
    }
  }

  // Slow path, must allocate.
  struct : public Locals {
    PinnedValue<JSObject> self;
    PinnedValue<> value;
    PinnedValue<PropStorage> storage;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScopeMarkerRAII marker{runtime};

  lv.self = self;
  lv.value = value.unboxToHV(runtime);

  // Load and modify storage, then store it back into self.
  if (!lv.self->propStorage_) {
    assert(indirectSlot == 0 && "allocated slot must be at end");
    auto arrRes = runtime.ignoreAllocationFailure(
        PropStorage::create(runtime, DEFAULT_PROPERTY_CAPACITY));
    lv.storage.castAndSetHermesValue<PropStorage>(arrRes);
    // It's likely that the capacity will be enough to use pushWithinCapacity,
    // but encoding the value back into SHV may allocate and trim the storage.
    // Avoid problems by using push_back.
    lv.storage->push_back(lv.storage, runtime, lv.value);
  } else {
    lv.storage = lv.self->propStorage_.getNonNull(runtime);
    // We know that the capacity is already not high enough due to above checks,
    // so don't do the check twice. Just call the slow path.
    PropStorage::pushBackSlowPath(lv.storage, runtime, lv.value);
  }

  lv.self->propStorage_.setNonNull(runtime, *lv.storage, runtime.getHeap());
}

CallResult<bool> JSObject::updateOwnProperty(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SymbolID name,
    HiddenClass::PropertyPos propertyPos,
    NamedPropertyDescriptor desc,
    const DefinePropertyFlags dpFlags,
    Handle<> valueOrAccessor,
    PropOpFlags opFlags) {
  auto updateStatus = checkPropertyUpdate(
      runtime,
      desc.flags,
      dpFlags,
      getNamedSlotValueUnsafe(selfHandle.get(), runtime, desc)
          .unboxToHV(runtime),
      valueOrAccessor,
      opFlags);
  if (updateStatus == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  if (updateStatus->first == PropertyUpdateStatus::failed)
    return false;

  // If the property flags changed, update them.
  if (updateStatus->second != desc.flags) {
    desc.flags = updateStatus->second;
    auto newClazz = HiddenClass::updateProperty(
        runtime.makeHandle(selfHandle->getClassGCPtr()),
        runtime,
        propertyPos,
        desc.flags);
    selfHandle->updateClass(runtime, *newClazz);
  }

  if (updateStatus->first == PropertyUpdateStatus::done)
    return true;
  assert(
      updateStatus->first == PropertyUpdateStatus::needSet &&
      "unexpected PropertyUpdateStatus");

  if (dpFlags.setValue) {
    if (LLVM_LIKELY(!desc.flags.internalSetter)) {
      auto shv = SmallHermesValue::encodeHermesValue(*valueOrAccessor, runtime);
      setNamedSlotValueUnsafe(selfHandle.get(), runtime, desc, shv);
    } else {
      return internalSetter(
          selfHandle, runtime, name, desc, valueOrAccessor, opFlags);
    }
  } else if (dpFlags.isAccessor()) {
    auto shv = SmallHermesValue::encodeHermesValue(*valueOrAccessor, runtime);
    setNamedSlotValueUnsafe(selfHandle.get(), runtime, desc, shv);
  } else {
    // If checkPropertyUpdate() returned needSet, but there is no value or
    // accessor, clear the value.
    setNamedSlotValueUnsafe(
        selfHandle.get(),
        runtime,
        desc,
        SmallHermesValue::encodeUndefinedValue());
  }

  return true;
}

CallResult<std::pair<JSObject::PropertyUpdateStatus, PropertyFlags>>
JSObject::checkPropertyUpdate(
    Runtime &runtime,
    const PropertyFlags currentFlags,
    DefinePropertyFlags dpFlags,
    const HermesValue curValueOrAccessor,
    Handle<> valueOrAccessor,
    PropOpFlags opFlags) {
  // 8.12.9 [5] Return true, if every field in Desc is absent.
  if (dpFlags.isEmpty())
    return std::make_pair(PropertyUpdateStatus::done, currentFlags);

  // The InternalForce() flag indicate the property update should go through, no
  // matter what. This is used by internal properties, which are often
  // implementation details and not real JS properties.
  if (opFlags.getInternalForce()) {
    return std::make_pair(PropertyUpdateStatus::needSet, currentFlags);
  }

  assert(
      (!dpFlags.isAccessor() || (!dpFlags.setWritable && !dpFlags.writable)) &&
      "can't set both accessor and writable");
  assert(
      !dpFlags.enableInternalSetter &&
      "cannot change the value of internalSetter");

  // 8.12.9 [6] Return true, if every field in Desc also occurs in current and
  // the value of every field in Desc is the same value as the corresponding
  // field in current when compared using the SameValue algorithm (9.12).
  // TODO: this would probably be much more efficient with bitmasks.
  if ((!dpFlags.setEnumerable ||
       dpFlags.enumerable == currentFlags.enumerable) &&
      (!dpFlags.setConfigurable ||
       dpFlags.configurable == currentFlags.configurable)) {
    if (dpFlags.isAccessor()) {
      if (currentFlags.accessor) {
        auto *curAccessor = vmcast<PropertyAccessor>(curValueOrAccessor);
        auto *newAccessor = vmcast<PropertyAccessor>(valueOrAccessor.get());

        if ((!dpFlags.setGetter ||
             curAccessor->getter == newAccessor->getter) &&
            (!dpFlags.setSetter ||
             curAccessor->setter == newAccessor->setter)) {
          return std::make_pair(PropertyUpdateStatus::done, currentFlags);
        }
      }
    } else {
      if (!currentFlags.accessor &&
          (!dpFlags.setValue ||
           isSameValue(curValueOrAccessor, valueOrAccessor.get())) &&
          (!dpFlags.setWritable || dpFlags.writable == currentFlags.writable)) {
        return std::make_pair(PropertyUpdateStatus::done, currentFlags);
      }
    }
  }

  // 8.12.9 [7]
  // If the property is not configurable, some aspects are not changeable.
  if (!currentFlags.configurable) {
    // Trying to change non-configurable to configurable?
    if (dpFlags.configurable) {
      if (opFlags.getThrowOnError()) {
        return runtime.raiseTypeError(
            "property is not configurable"); // TODO: better message.
      }
      return std::make_pair(PropertyUpdateStatus::failed, PropertyFlags{});
    }

    // Trying to change the enumerability of non-configurable property?
    if (dpFlags.setEnumerable &&
        dpFlags.enumerable != currentFlags.enumerable) {
      if (opFlags.getThrowOnError()) {
        return runtime.raiseTypeError(
            "property is not configurable"); // TODO: better message.
      }
      return std::make_pair(PropertyUpdateStatus::failed, PropertyFlags{});
    }
  }

  PropertyFlags newFlags = currentFlags;

  // 8.12.9 [8] If IsGenericDescriptor(Desc) is true, then no further validation
  // is required.
  if (!(dpFlags.setValue || dpFlags.setWritable || dpFlags.setGetter ||
        dpFlags.setSetter)) {
    // Do nothing
  }
  // 8.12.9 [9]
  // Changing between accessor and data descriptor?
  else if (currentFlags.accessor != dpFlags.isAccessor()) {
    if (!currentFlags.configurable) {
      if (opFlags.getThrowOnError()) {
        return runtime.raiseTypeError(
            "property is not configurable"); // TODO: better message.
      }
      return std::make_pair(PropertyUpdateStatus::failed, PropertyFlags{});
    }

    // If we change from accessor to data descriptor, Preserve the existing
    // values of the converted property’s [[Configurable]] and [[Enumerable]]
    // attributes and set the rest of the property’s attributes to their default
    // values.
    // If it's the other way around, since the accessor doesn't have the
    // [[Writable]] attribute, do nothing.
    newFlags.writable = 0;

    // If we are changing from accessor to non-accessor, we must set a new
    // value.
    if (!dpFlags.isAccessor())
      dpFlags.setValue = 1;
  }
  // 8.12.9 [10] if both are data descriptors.
  else if (!currentFlags.accessor) {
    if (!currentFlags.configurable) {
      if (!currentFlags.writable) {
        // If the current property is not writable, but the new one is.
        if (dpFlags.writable) {
          if (opFlags.getThrowOnError()) {
            return runtime.raiseTypeError(
                "property is not configurable"); // TODO: better message.
          }
          return std::make_pair(PropertyUpdateStatus::failed, PropertyFlags{});
        }

        // If we are setting a different value.
        if (dpFlags.setValue &&
            !isSameValue(curValueOrAccessor, valueOrAccessor.get())) {
          if (opFlags.getThrowOnError()) {
            return runtime.raiseTypeError(
                "property is not writable"); // TODO: better message.
          }
          return std::make_pair(PropertyUpdateStatus::failed, PropertyFlags{});
        }
      }
    }
  }
  // 8.12.9 [11] Both are accessors.
  else {
    auto *curAccessor = vmcast<PropertyAccessor>(curValueOrAccessor);
    auto *newAccessor = vmcast<PropertyAccessor>(valueOrAccessor.get());

    // If not configurable, make sure that nothing is changing.
    if (!currentFlags.configurable) {
      if ((dpFlags.setGetter && newAccessor->getter != curAccessor->getter) ||
          (dpFlags.setSetter && newAccessor->setter != curAccessor->setter)) {
        if (opFlags.getThrowOnError()) {
          return runtime.raiseTypeError(
              "property is not configurable"); // TODO: better message.
        }
        return std::make_pair(PropertyUpdateStatus::failed, PropertyFlags{});
      }
    }

    // If not setting the getter or the setter, re-use the current one.
    if (!dpFlags.setGetter)
      newAccessor->getter.set(runtime, curAccessor->getter, runtime.getHeap());
    if (!dpFlags.setSetter)
      newAccessor->setter.set(runtime, curAccessor->setter, runtime.getHeap());
  }

  // 8.12.9 [12] For each attribute field of Desc that is present, set the
  // correspondingly named attribute of the property named P of object O to the
  // value of the field.
  if (dpFlags.setEnumerable)
    newFlags.enumerable = dpFlags.enumerable;
  if (dpFlags.setWritable)
    newFlags.writable = dpFlags.writable;
  if (dpFlags.setConfigurable)
    newFlags.configurable = dpFlags.configurable;

  if (dpFlags.setValue)
    newFlags.accessor = false;
  else if (dpFlags.isAccessor())
    newFlags.accessor = true;
  else
    return std::make_pair(PropertyUpdateStatus::done, newFlags);

  return std::make_pair(PropertyUpdateStatus::needSet, newFlags);
}

CallResult<bool> JSObject::internalSetter(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SymbolID name,
    NamedPropertyDescriptor /*desc*/,
    Handle<> value,
    PropOpFlags opFlags) {
  if (vmisa<JSArray>(selfHandle.get())) {
    if (name == Predefined::getSymbolID(Predefined::length)) {
      return JSArray::setLength(
          Handle<JSArray>::vmcast(selfHandle), runtime, value, opFlags);
    }
  }

  llvm_unreachable("unhandled property in Object::internalSetter()");
}

namespace {

/// Helper function to add all the property names of an object to an
/// array, starting at the given index.
/// All string keys are added as Symbols, to aid lookup in GetNextPName.
/// All number keys are added as numbers.
/// Only enumerable properties are included.
/// Write the number of object properties for the obj to index 1 of \p arr
/// (does not include proto properties).
/// Returns the index after the last property added.
CallResult<uint32_t> appendAllPropertyKeys(
    Handle<JSObject> obj,
    Runtime &runtime,
    MutableHandle<ArrayStorageSmall> arr,
    uint32_t beginIndex,
    OwnKeysFlags okFlags) {
  struct : public Locals {
    PinnedValue<> prop;
    PinnedValue<StringPrimitive> tmpString;
    PinnedValue<SymbolID> propId;
    PinnedValue<JSObject> head;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  uint32_t size = beginIndex;
  // We know that duplicate property names can only exist between objects in
  // the prototype chain. Hence there should not be duplicated properties
  // before we start to look at any prototype.
  bool needDedup = false;
  lv.head = obj;

  // Make sure we can store SymbolID::RawType in DenseSet.
  assert(
      llvh::DenseMapInfo<SymbolID::RawType>::getEmptyKey() >=
          SymbolID::FIRST_INVALID_ID &&
      "can't store SymbolID::RawType in DenseSet");
  assert(
      llvh::DenseMapInfo<SymbolID::RawType>::getTombstoneKey() >=
          SymbolID::FIRST_INVALID_ID &&
      "can't store SymbolID::RawType in DenseSet");

  // Keep track of the unique props we have seen so far. The props may be
  // strings (SymbolIDs) or index names.
  llvh::SmallDenseSet<SymbolID::RawType, 8> dedupNames;
  // Store uint64_t in the set here to avoid collisions between tombstone/empty
  // keys and real indices.
  // In reality, all inserted keys will be valid array indices, meaning they
  // will fit in 32 bits.
  llvh::SmallDenseSet<uint64_t, 8> dedupIdxNames;
  // Add the current value of prop and/or propIdHandle to correct set(s).
  // Always called after property is added to \c arr, so the symbols in
  // dedupNames stay alive.
  auto addToDedup = [&lv, &dedupIdxNames, &dedupNames, &runtime](
                        Handle<> prop, Handle<SymbolID> propIdHandle) {
    if (prop->isNumber()) {
      assert(
          doubleToArrayIndex(prop->getNumber()) &&
          "getOwnPropertyKeys should only return numbers for indices");
      uint32_t d = prop->getNumberAs<uint32_t>();
      dedupIdxNames.insert(d);
    } else {
      SymbolID sym = propIdHandle.get();
      dedupNames.insert(sym.unsafeGetRaw());
      // We need to make sure that we check for duplicates across the number and
      // string types. This is because 3 should be treated as a duplicate of
      // '3'. Therefore, we attempt to convert this string to a number, and
      // insert the resulting value in the duplicate set.
      lv.tmpString = runtime.getStringPrimFromSymbolID(*propIdHandle);
      OptValue<uint32_t> strToIdx = toArrayIndex(*lv.tmpString);
      if (LLVM_UNLIKELY(strToIdx)) {
        dedupIdxNames.insert(*strToIdx);
      }
    }
  }; // end of lambda expression
  for (bool first = true; lv.head.get(); first = false) {
    GCScope gcScope(runtime);

    // enumerableProps will contain all enumerable own properties from obj.
    // Impl note: this is the only place where getOwnPropertyKeys will be
    // called without IncludeNonEnumerable on a Proxy.  Everywhere else,
    // trap ordering is specified but ES9 13.7.5.15 says "The mechanics and
    // order of enumerating the properties is not specified", which is
    // unusual.
    auto cr = JSObject::getOwnPropertyKeys(lv.head, runtime, okFlags);
    if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto enumerableProps = *cr;
    auto marker = gcScope.createMarker();
    if (first && beginIndex > 0) {
      auto endIndex = SmallHermesValue::encodeNumberValue(
          enumerableProps->getEndIndex(), runtime);
      arr->set(1, endIndex, runtime.getHeap());
    }
    for (unsigned i = 0, e = enumerableProps->getEndIndex(); i < e; ++i) {
      gcScope.flushToMarker(marker);
      lv.prop = enumerableProps->at(runtime, i).unboxToHV(runtime);
      assert(
          (lv.prop->isNumber() || lv.prop->isString() || lv.prop->isSymbol()) &&
          "property name is not a string or number");
      if (lv.prop->isSymbol()) {
        lv.propId = lv.prop->getSymbol();
      } else if (lv.prop->isString()) {
        CallResult<Handle<SymbolID>> symRes =
            runtime.getIdentifierTable().getSymbolHandleFromPrimitive(
                runtime, createPseudoHandle(lv.prop->getString()));
        if (LLVM_UNLIKELY(symRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        lv.propId = *symRes;
      }
      if (!needDedup) {
        // If no dedup is needed, add it directly.
        if (LLVM_UNLIKELY(
                ArrayStorageSmall::push_back(arr, runtime, lv.prop) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        addToDedup(lv.prop, lv.propId);
        ++size;
        continue;
      }
      // Otherwise check the existing sets for matches.
      bool dupFound;
      if (lv.prop->isNumber()) {
        dupFound = dedupIdxNames.count(lv.prop->getNumber());
      } else {
        assert(
            (lv.prop->isString() || lv.prop->isSymbol()) &&
            "getOwnPropertyKeys only returns symbol, string, or number");
        dupFound = dedupNames.count(lv.propId.get().unsafeGetRaw());

        lv.tmpString = lv.prop->isString()
            ? lv.prop->getString()
            : runtime.getStringPrimFromSymbolID(*lv.propId);

        // If we still haven't found a duplicate and there have been previous
        // index names, then attempt to convert this string prop to an index and
        // check that number value for duplicates.
        if (LLVM_UNLIKELY(!dupFound && !dedupIdxNames.empty())) {
          OptValue<uint32_t> propNum = toArrayIndex(*lv.tmpString);
          if (LLVM_UNLIKELY(propNum))
            dupFound = dedupIdxNames.count(*propNum);
        }
      }
      if (LLVM_LIKELY(!dupFound)) {
        if (LLVM_UNLIKELY(
                ArrayStorageSmall::push_back(arr, runtime, lv.prop) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        addToDedup(lv.prop, lv.propId);
        ++size;
      }
    }
    // Continue to follow the prototype chain.
    CallResult<PseudoHandle<JSObject>> parentRes =
        JSObject::getPrototypeOf(createPseudoHandle(*lv.head), runtime);
    if (LLVM_UNLIKELY(parentRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.head = parentRes->get();
    needDedup = true;
  }
  return size;
}

/// Adds the hidden classes of the prototype chain of obj to arr,
/// starting with the class of obj at index 2, etc., and
/// terminates with null.
/// Index 0 will contain the end index after this is complete.
/// Index 1 will contain the number of properties pushed in obj.
/// Index 2 will contain the class of obj.
///
/// \param obj The object whose prototype chain should be output
/// \param[out] arr The array where the classes will be appended. This
/// array is cleared if any object is unsuitable for caching.
ExecutionStatus setProtoClasses(
    Runtime &runtime,
    Handle<JSObject> obj,
    MutableHandle<ArrayStorageSmall> arr) {
  // Layout of a JSArray stored in the for-in cache:
  // [numProtos, numObjProps, class(obj),
  // class(proto(obj)), class(proto(proto(obj))), ..., null,
  // prop0, prop1, ...]
  // prop0 is at index numProtos.
  // There are numObjProps properties on obj before the proto properties start.

  if (!obj->shouldCacheForIn(runtime)) {
    arr->clear(runtime);
    return ExecutionStatus::RETURNED;
  }
  MutableHandle<JSObject> head(runtime, *obj);
  MutableHandle<> clazz(runtime);
  GCScopeMarkerRAII marker{runtime};
  // Push entries 0 and 1 (we'll fill them in later).
  clazz = HermesValue::encodeTrustedNumberValue(0);
  if (LLVM_UNLIKELY(
          ArrayStorageSmall::push_back(arr, runtime, clazz) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_UNLIKELY(
          ArrayStorageSmall::push_back(arr, runtime, clazz) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  while (head.get()) {
    if (!head->shouldCacheForIn(runtime)) {
      arr->clear(runtime);
      return ExecutionStatus::RETURNED;
    }
    if (head->isLazy(runtime)) {
      // Ensure all properties have been initialized before caching the hidden
      // class. Not doing this will result in changes to the hidden class
      // when getOwnPropertyKeys is called later.
      JSObject::initializeLazyObject(runtime, head);
    }
    clazz = HermesValue::encodeObjectValue(head->getClass(runtime));
    if (LLVM_UNLIKELY(
            ArrayStorageSmall::push_back(arr, runtime, clazz) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    head = head->getParent(runtime);
    marker.flush();
  }
  clazz = HermesValue::encodeNullValue();
  if (LLVM_UNLIKELY(
          ArrayStorageSmall::push_back(arr, runtime, clazz) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  auto endIndex = SmallHermesValue::encodeNumberValue(arr->size(), runtime);
  arr->set(0, endIndex, runtime.getHeap());
  return ExecutionStatus::RETURNED;
}

/// Verifies that the classes of obj's prototype chain still matches those
/// previously prefixed to arr by setProtoClasses.
/// Assumes that the first elements are the count of HiddenClasses and the count
/// of the object's own properties, and element 2 is is the object's own
/// HiddenClass, so it doesn't check those.
///
/// \param obj The object whose prototype chain should be verified
/// \param arr Array previously populated by setProtoClasses
/// \return The index after the terminating null if everything matches,
/// otherwise 0.
uint32_t matchesProtoClasses(
    Runtime &runtime,
    Handle<JSObject> obj,
    Handle<ArrayStorageSmall> arr) {
  MutableHandle<JSObject> head(runtime, obj->getParent(runtime));
  // Skip the counts and object's own class.
  uint32_t i = 3;
  while (head.get()) {
    auto protoCls = arr->at(i++);
    if (protoCls.isNull() || protoCls.getObject() != head->getClassGCPtr() ||
        head->isProxyObject(runtime)) {
      return 0;
    }
    head = head->getParent(runtime);
  }
  // The chains must both end at the same point.
  if (head || !arr->at(i++).isNull()) {
    return 0;
  }
  assert(i > 0 && "success should be positive");
  return i;
}

} // namespace

CallResult<Handle<ArrayStorageSmall>> getForInPropertyNames(
    Runtime &runtime,
    Handle<JSObject> obj,
    uint32_t &beginIndex,
    uint32_t &endIndex) {
  Handle<HiddenClass> clazz = runtime.makeHandle(obj->getClass(runtime));

  // Fast case: Check the cache.
  MutableHandle<ArrayStorageSmall> arr(runtime);
  if (obj->shouldCacheForIn(runtime)) {
    arr = clazz->getForInCache(runtime);
    if (arr) {
      beginIndex = matchesProtoClasses(runtime, obj, arr);
      if (beginIndex) {
        // Cache is valid for this object, so use it.
        endIndex = arr->size();
        return arr;
      }
      // Invalid for this object. We choose to clear the cache since the
      // changes to the prototype chain probably affect other objects too.
      clazz->clearForInCache(runtime);
      // Clear arr to slightly reduce risk of OOM from allocation below.
      arr = nullptr;
    }
  }

  // Slow case: Build the array of properties.
  auto ownPropEstimate = clazz->getNumProperties();
  auto arrRes = obj->shouldCacheForIn(runtime)
      ? ArrayStorageSmall::createLongLived(runtime, ownPropEstimate)
      : ArrayStorageSmall::create(runtime, ownPropEstimate);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  arr = vmcast<ArrayStorageSmall>(*arrRes);
  if (setProtoClasses(runtime, obj, arr) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  beginIndex = arr->size();
  // If obj or any of its prototypes are unsuitable for caching, then
  // beginIndex is 0 and we return an array with only the property names.
  bool canCache = beginIndex;
  auto end = appendAllPropertyKeys(
      obj,
      runtime,
      arr,
      beginIndex,
      OwnKeysFlags().plusIncludeNonSymbols().plusKeepSymbols());
  if (end == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  endIndex = *end;
  // Avoid degenerate memory explosion: if > 75% of the array is properties
  // or classes from prototypes, then don't cache it.
  const bool tooMuchProto = *end / 4 > ownPropEstimate;
  if (canCache && !tooMuchProto) {
    assert(beginIndex > 0 && "cached array must start with proto classes");
#ifdef HERMES_SLOW_DEBUG
    assert(beginIndex == matchesProtoClasses(runtime, obj, arr) && "matches");
#endif
    clazz->setForInCache(*arr, runtime);
  }
  return arr;
}

CallResult<PseudoHandle<>> getIndexedWithReceiver_RJS(
    Runtime &runtime,
    Handle<JSObject> srcHandle,
    uint64_t index,
    Handle<> receiver) {
  if (auto optRes = tryFastGetComputedMayAlloc(runtime, *srcHandle, index))
    return createPseudoHandle(*optRes);

  PinnedValue<> hvIndex = PinnedHermesValue::encodeTrustedNumberValue(index);
  // TODO: call a less general function?
  return JSObject::getComputedWithReceiver_RJS(
      srcHandle, runtime, hvIndex, receiver);
}

} // namespace vm
} // namespace hermes

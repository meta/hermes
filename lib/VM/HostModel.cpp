/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/HostModel.h"

#include "hermes/VM/BuildMetadata.h"

namespace hermes {
namespace vm {

//===----------------------------------------------------------------------===//
// class FinalizableNativeFunction

const CallableVTable FinalizableNativeFunction::vt{
    {
        VTable(
            CellKind::FinalizableNativeFunctionKind,
            cellSize<FinalizableNativeFunction>(),
            /* allowLargeAlloc */ false,
            FinalizableNativeFunction::_finalizeImpl),
        FinalizableNativeFunction::_getOwnIndexedRangeImpl,
        FinalizableNativeFunction::_haveOwnIndexedImpl,
        FinalizableNativeFunction::_getOwnIndexedPropertyFlagsImpl,
        FinalizableNativeFunction::_getOwnIndexedImpl,
        FinalizableNativeFunction::_setOwnIndexedImpl,
        FinalizableNativeFunction::_deleteOwnIndexedImpl,
        FinalizableNativeFunction::_checkAllOwnIndexedImpl,
    },
    FinalizableNativeFunction::_callImpl};

CallResult<PseudoHandle<>> FinalizableNativeFunction::_callImpl(
    Handle<Callable> selfHandle,
    Runtime &runtime) {
  CallResult<PseudoHandle<>> res =
      _nativeCall(vmcast<NativeFunction>(selfHandle.get()), runtime);
  if (LLVM_UNLIKELY(res != ExecutionStatus::EXCEPTION)) {
    StackFramePtr newFrame{runtime.getStackPointer()};
    bool isConstructCall = !newFrame.getNewTargetRef().isUndefined();
    // FinalizableNativeFunction must return an object value when it's called as
    // a constructor.
    if (isConstructCall && !(*res)->isObject()) {
      return runtime.raiseTypeError(
          "FinalizableNativeFunction constructor must return an object");
    }
  }
  return res;
}

void FinalizableNativeFunctionBuildMeta(
    const GCCell *cell,
    Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(
      JSObject::numOverlapSlots<FinalizableNativeFunction>());
  NativeFunctionBuildMeta(cell, mb);
  mb.setVTable(&FinalizableNativeFunction::vt);
}

HiddenClass *FinalizableNativeFunction::createClass(
    Runtime &runtime,
    Handle<HiddenClass> rootClazz) {
  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  // HiddenClass::addProperty() returns each new class in a new handle. Free
  // those handles here, so that they do not stay in the caller's GCScope.
  GCScopeMarkerRAII marker{runtime};

  // Same flags as Callable::defineNameLengthAndPrototype().
  PropertyFlags pf{};
  pf.enumerable = 0;
  pf.writable = 0;
  pf.configurable = 1;

  auto addedLength = runtime.ignoreAllocationFailure(
      HiddenClass::addProperty(
          rootClazz, runtime, Predefined::getSymbolID(Predefined::length), pf));
  assert(
      addedLength.second == lengthPropIndex() &&
      "FinalizableNativeFunction.length has invalid index");
  lv.clazz = addedLength.first;

  auto addedName = runtime.ignoreAllocationFailure(
      HiddenClass::addProperty(
          lv.clazz, runtime, Predefined::getSymbolID(Predefined::name), pf));
  assert(
      addedName.second == namePropIndex() &&
      "FinalizableNativeFunction.name has invalid index");
  lv.clazz = addedName.first;

  return *lv.clazz;
}

CallResult<HermesValue> FinalizableNativeFunction::createWithoutPrototype(
    Runtime &runtime,
    void *context,
    NativeFunctionPtr functionPtr,
    FinalizeNativeFunctionPtr finalizePtr,
    SymbolID name,
    unsigned paramCount) {
  assert(name.isValid() && "A name must always be provided");

  struct : public Locals {
    PinnedValue<FinalizableNativeFunction> self;
    PinnedValue<StringPrimitive> name;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // The class already has 'length' and 'name', and its object parent is
  // Function.prototype. So the new object needs no class transitions, only
  // the values of the two properties.
  auto *cell = runtime.makeAFixed<FinalizableNativeFunction, HasFinalizer::Yes>(
      runtime,
      Handle<JSObject>::vmcast(&runtime.functionPrototype),
      Handle<HiddenClass>::vmcast(
          &runtime.classFinalizableNativeFunctionNameLength),
      context,
      functionPtr,
      finalizePtr);
  lv.self = JSObjectInit::initToPointer(runtime, cell);

  // When the overlap slots use all the direct property slots (for example,
  // with compressed pointers), the two properties need indirect storage. Give
  // it the default capacity, as JSObject::allocateNewSlotStorage() does for
  // the first indirect property. Like there, an allocation failure is fatal:
  // the cell owns the context now, so this function must not fail.
  constexpr SlotIndex kNumSlots = namePropIndex() + 1;
  constexpr PropStorage::size_type kNumIndirect =
      kNumSlots > DIRECT_PROPERTY_SLOTS ? kNumSlots - DIRECT_PROPERTY_SLOTS : 0;
  static_assert(
      kNumIndirect <= DEFAULT_PROPERTY_CAPACITY,
      "Indirect properties must fit in the default capacity");
  if constexpr (kNumIndirect > 0) {
    HermesValue storage = runtime.ignoreAllocationFailure(
        PropStorage::create(runtime, DEFAULT_PROPERTY_CAPACITY, kNumIndirect));
    lv.self->propStorage_.setNonNull(
        runtime, vmcast<PropStorage>(storage), runtime.getHeap());
  }

  lv.name = runtime.getStringPrimFromSymbolID(name);

  // Encode the length before reading the object pointer: with boxed doubles,
  // encoding a number can allocate, and so move the object.
  SmallHermesValue lengthSHV =
      SmallHermesValue::encodeNumberValue(paramCount, runtime);
  setNamedSlotValueUnsafe(lv.self.get(), runtime, lengthPropIndex(), lengthSHV);
  setNamedSlotValueUnsafe(
      lv.self.get(),
      runtime,
      namePropIndex(),
      SmallHermesValue::encodeStringValue(lv.name.get(), runtime));

  return lv.self.getHermesValue();
}

HostObjectProxy::~HostObjectProxy() {}

const ObjectVTable HostObject::vt{
    VTable(
        CellKind::HostObjectKind,
        cellSize<HostObject>(),
        /* allowLargeAlloc */ false,
        HostObject::_finalizeImpl),
    HostObject::_getOwnIndexedRangeImpl,
    HostObject::_haveOwnIndexedImpl,
    HostObject::_getOwnIndexedPropertyFlagsImpl,
    HostObject::_getOwnIndexedImpl,
    HostObject::_setOwnIndexedImpl,
    HostObject::_deleteOwnIndexedImpl,
    HostObject::_checkAllOwnIndexedImpl,
};

void HostObjectBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<HostObject>());
  JSObjectBuildMeta(cell, mb);
  mb.setVTable(&HostObject::vt);
}

CallResult<HermesValue> HostObject::createWithoutPrototype(
    Runtime &runtime,
    std::unique_ptr<HostObjectProxy> proxy) {
  auto parentHandle = Handle<JSObject>::vmcast(&runtime.objectPrototype);

  HostObject *hostObj = runtime.makeAFixed<HostObject, HasFinalizer::Yes>(
      runtime,
      parentHandle,
      runtime.getHiddenClassForPrototype(
          *parentHandle, runtime.classHostObject),
      std::move(proxy));

  assert(
      hostObj->isHostObject(runtime) &&
      "new HostObject made with the wrong HiddenClass");

  return JSObjectInit::initToHermesValue(runtime, hostObj);
}

} // namespace vm
} // namespace hermes

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

CallResult<HermesValue> FinalizableNativeFunction::createWithoutPrototype(
    Runtime &runtime,
    void *context,
    NativeFunctionPtr functionPtr,
    FinalizeNativeFunctionPtr finalizePtr,
    SymbolID name,
    unsigned paramCount) {
  auto parentHandle = Handle<JSObject>::vmcast(&runtime.functionPrototype);

  auto *cell = runtime.makeAFixed<FinalizableNativeFunction, HasFinalizer::Yes>(
      runtime,
      parentHandle,
      runtime.getHiddenClassForPrototype(
          *parentHandle, runtime.classFinalizableNativeFunction),
      context,
      functionPtr,
      finalizePtr);
  struct : public Locals {
    PinnedValue<FinalizableNativeFunction> self;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.self = JSObjectInit::initToPseudoHandle(runtime, cell);

  auto prototypeObjectHandle = runtime.makeNullHandle<JSObject>();

  auto st = defineNameLengthAndPrototype(
      lv.self,
      runtime,
      name,
      paramCount,
      prototypeObjectHandle,
      Callable::WritablePrototype::Yes);
  (void)st;
  assert(
      st != ExecutionStatus::EXCEPTION && "defineLengthAndPrototype() failed");

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

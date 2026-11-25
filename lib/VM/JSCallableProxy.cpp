/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JSCallableProxy.h"

#include "hermes/VM/JSArray.h"

namespace hermes {
namespace vm {

//===----------------------------------------------------------------------===//
// class JSCallableProxy

const CallableVTable JSCallableProxy::vt{
    {
        VTable(CellKind::JSCallableProxyKind, cellSize<JSCallableProxy>()),
        JSCallableProxy::_getOwnIndexedRangeImpl,
        JSCallableProxy::_haveOwnIndexedImpl,
        JSCallableProxy::_getOwnIndexedPropertyFlagsImpl,
        JSCallableProxy::_getOwnIndexedImpl,
        JSCallableProxy::_setOwnIndexedImpl,
        JSCallableProxy::_deleteOwnIndexedImpl,
        JSCallableProxy::_checkAllOwnIndexedImpl,
    },
    JSCallableProxy::_callImpl,
};

void JSCallableProxyBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<JSCallableProxy>());
  NativeFunctionBuildMeta(cell, mb);
  const auto *self = static_cast<const JSCallableProxy *>(cell);
  mb.setVTable(&JSCallableProxy::vt);
  mb.addField("@target", &self->slots_.target);
  mb.addField("@handler", &self->slots_.handler);
}

PseudoHandle<JSCallableProxy> JSCallableProxy::create(Runtime &runtime) {
  auto *cproxy = runtime.makeAFixed<JSCallableProxy>(
      runtime,
      HandleRootOwner::makeNullHandle<JSObject>(),
      runtime.getHiddenClassForPrototype(
          nullptr, runtime.classJSCallableProxy));

  assert(
      cproxy->isProxyObject(runtime) &&
      "new Proxy made with the wrong HiddenClass");

  return JSObjectInit::initToPseudoHandle(runtime, cproxy);
}

void JSCallableProxy::setTargetAndHandler(
    Runtime &runtime,
    Handle<JSObject> target,
    Handle<JSObject> handler) {
  slots_.target.set(runtime, target.get(), runtime.getHeap());
  slots_.handler.set(runtime, handler.get(), runtime.getHeap());
}

CallResult<HermesValue> JSCallableProxy::_proxyNativeCall(
    void *,
    Runtime &runtime) {
  GCScope gcScope(runtime);
  // We don't use NativeArgs; the implementations just read the current
  // stack frame directly.
  StackFramePtr callerFrame = runtime.getCurrentFrame();
  auto selfHandle =
      Handle<JSCallableProxy>::vmcast(&callerFrame.getCalleeClosureOrCBRef());
  // detail::slots() will always return the slots_ field here, but
  // this isn't a member, so it can't see it.  It doesn't seem to be
  // worth tweaking the abstractions to avoid the small overhead in
  // this case.
  // Make sure to retrieve the target and handler before any JS can execute.
  bool isConstructorCall = callerFrame->isConstructorCall();
  struct : public Locals {
    PinnedValue<> thisArg;
    PinnedValue<JSObject> target;
    PinnedValue<JSObject> handler;
    PinnedValue<JSArray> argArray;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  // We do the constructor related work of checking for a valid call and making
  // `this` here, before obtaining the slots for the actual call operation. In
  // the spec, this work happens first so it's important to keep the order. For
  // instance, if obtaining the `.prototype` of the new.target during
  // ordinaryCreateFromConstructor ends up revoking this proxy, then we should
  // throw an error when we attempt to find the "construct" trap.
  if (isConstructorCall) {
    // The interpreter won't verify if a construct call to a proxy is valid, so
    // we do that validation here. We should check before we run any user JS
    // from findTrap.
    if (!(isConstructor(runtime, *selfHandle) &&
          isConstructor(runtime, callerFrame->getNewTargetRef()))) {
      return runtime.raiseTypeError("Function is not a constructor");
    }
    // When called as a constructor, JSCallableProxy does not receive a
    // populated `this` parameter. So we create it here.
    auto thisArgRes = ordinaryCreateFromConstructor_RJS(
        runtime,
        Handle<Callable>::vmcast(&callerFrame->getNewTargetRef()),
        runtime.objectPrototype);
    if (LLVM_UNLIKELY(thisArgRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.thisArg = std::move(*thisArgRes);
  } else {
    lv.thisArg = callerFrame.getThisArgRef();
  }
  auto &slots = detail::slots(*selfHandle);
  lv.target = slots.target.get(runtime);
  lv.handler = slots.handler.get(runtime);
  Predefined::Str trapName =
      isConstructorCall ? Predefined::construct : Predefined::apply;
  CallResult<Handle<Callable>> trapRes =
      detail::findTrap(selfHandle, runtime, trapName);
  if (trapRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 6. If trap is undefined, then
  if (!*trapRes) {
    //   a. Return ? Call(target, thisArgument, argumentsList).
    // OR
    //   a. Assert: IsConstructor(target) is true.
    //   b. Return ? Construct(target, argumentsList, newTarget).
    ScopedNativeCallFrame newFrame{
        runtime,
        callerFrame.getArgCount(),
        lv.target.getHermesValue(),
        callerFrame.getNewTargetRef(),
        lv.thisArg.getHermesValue()};
    if (LLVM_UNLIKELY(newFrame.overflowed()))
      return runtime.raiseStackOverflow(
          Runtime::StackOverflowKind::NativeStack);
    std::uninitialized_copy_n(
        callerFrame.argsBegin(),
        callerFrame.getArgCount(),
        newFrame->argsBegin());
    // I know statically that target is a Callable, but storing it as
    // a Callable makes it much harder to share all the JSProxy code,
    // so we cast here.
    auto res = Callable::call(
        Handle<Callable>::vmcast(Handle<JSObject>(lv.target)), runtime);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // Constructor calls *must* return an object. If the returned value is not
    // an object, choose the `this` we created. Non-construct calls can just use
    // the value directly.
    if (!isConstructorCall) {
      return res->get();
    }
    return (*res)->isObject() ? res->get() : lv.thisArg.getHermesValue();
  }
  // 7. Let argArray be CreateArrayFromList(argumentsList).
  auto argArrayRes = JSArray::create(
      runtime, callerFrame->getArgCount(), callerFrame->getArgCount());
  if (LLVM_UNLIKELY(argArrayRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.argArray = std::move(*argArrayRes);
  JSArray::setStorageEndIndex(lv.argArray, runtime, callerFrame->getArgCount());
  for (uint32_t i = 0; i < callerFrame->getArgCount(); ++i) {
    const auto shv =
        SmallHermesValue::encodeHermesValue(callerFrame.getArgRef(i), runtime);
    JSArray::unsafeSetExistingElementAt(*lv.argArray, runtime, i, shv);
  }
  // 8. Let newObj be ? Call(trap, handler, « target, argArray, newTarget »).
  if (isConstructorCall) {
    CallResult<PseudoHandle<>> newObjRes = Callable::executeCall3(
        *trapRes,
        runtime,
        lv.handler,
        lv.target.getHermesValue(),
        lv.argArray.getHermesValue(),
        callerFrame.getNewTargetRef());
    if (LLVM_UNLIKELY(newObjRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // 9. If Type(newObj) is not Object, throw a TypeError exception.
    if (!vmisa<JSObject>(newObjRes->get())) {
      return runtime.raiseTypeError("Proxy construct trap returned non-Object");
    }
    // 10. return newObj.
    return newObjRes->get();
  } else {
    // 8. Return ? Call(trap, handler, « target, thisArgument, argArray »).
    auto res = Callable::executeCall3(
        *trapRes,
        runtime,
        lv.handler,
        lv.target.getHermesValue(),
        callerFrame.getThisArgRef(),
        lv.argArray.getHermesValue());
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return res->get();
  }
}

} // namespace vm
} // namespace hermes

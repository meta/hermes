/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JSProxy.h"

#include "hermes/VM/ArrayLike.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSCallableProxy.h"
#include "hermes/VM/JSMapImpl.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/PropertyAccessor.h"

#include "llvh/ADT/SetVector.h"

namespace hermes {
namespace vm {

namespace detail {

ProxySlots &slots(JSObject *self) {
  if (auto *proxy = dyn_vmcast<JSProxy>(self)) {
    return proxy->slots_;
  } else {
    auto *cproxy = dyn_vmcast<JSCallableProxy>(self);
    assert(
        cproxy && "JSProxy methods must be passed JSProxy or JSCallableProxy");
    return cproxy->slots_;
  }
}

CallResult<Handle<Callable>>
findTrap(Handle<JSObject> selfHandle, Runtime &runtime, Predefined::Str name) {
  // 2. Let handler be O.[[ProxyHandler]].
  // 3. If handler is null, throw a TypeError exception.
  JSObject *handlerPtr = detail::slots(*selfHandle).handler.get(runtime);
  if (!handlerPtr) {
    return runtime.raiseTypeError("Proxy handler is null");
  }
  // 4. Assert: Type(handler) is Object.
  // 5. Let target be O.[[ProxyTarget]].
  // 6. Let trap be ? GetMethod(handler, « name »).
  CallResult<PseudoHandle<>> trapVal = [&]() {
    GCScope gcScope(runtime);
    Handle<JSObject> handler = runtime.makeHandle(handlerPtr);
    return JSObject::getNamed_RJS(
        handler, runtime, Predefined::getSymbolID(name));
  }();

  if (trapVal == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  if ((*trapVal)->isUndefined() || (*trapVal)->isNull()) {
    return Runtime::makeNullHandle<Callable>();
  }

  if (!vmisa<Callable>(trapVal->get())) {
    return runtime.raiseTypeErrorForValue(
        runtime.makeHandle(std::move(*trapVal)),
        " is not a Proxy trap function");
  }
  return runtime.makeHandle<Callable>(trapVal->get());
}

} // namespace detail

//===----------------------------------------------------------------------===//
// class JSProxy

const ObjectVTable JSProxy::vt{
    VTable(CellKind::JSProxyKind, cellSize<JSProxy>()),
    JSProxy::_getOwnIndexedRangeImpl,
    JSProxy::_haveOwnIndexedImpl,
    JSProxy::_getOwnIndexedPropertyFlagsImpl,
    JSProxy::_getOwnIndexedImpl,
    JSProxy::_setOwnIndexedImpl,
    JSProxy::_deleteOwnIndexedImpl,
    JSProxy::_checkAllOwnIndexedImpl,
};

void JSProxyBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<JSProxy>());
  JSObjectBuildMeta(cell, mb);
  const auto *self = static_cast<const JSProxy *>(cell);
  mb.setVTable(&JSProxy::vt);
  mb.addField("@target", &self->slots_.target);
  mb.addField("@handler", &self->slots_.handler);
}

PseudoHandle<JSProxy> JSProxy::create(Runtime &runtime) {
  JSProxy *proxy = runtime.makeAFixed<JSProxy>(
      runtime,
      // Proxy should not have an observable prototype, so we just set it to
      // null.
      Runtime::makeNullHandle<JSObject>(),
      runtime.getHiddenClassForPrototype(nullptr, runtime.classJSProxy));

  assert(
      proxy->isProxyObject(runtime) &&
      "new Proxy made with the wrong HiddenClass");

  return JSObjectInit::initToPseudoHandle(runtime, proxy);
}

void JSProxy::setTargetAndHandler(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<JSObject> target,
    Handle<JSObject> handler) {
  auto &slots = detail::slots(*selfHandle);
  slots.target.set(runtime, target.get(), runtime.getHeap());
  slots.handler.set(runtime, handler.get(), runtime.getHeap());
}

namespace {

void completePropertyDescriptor(DefinePropertyFlags &desc) {
  if ((desc.setValue || desc.setWritable) ||
      (!desc.setGetter && !desc.setSetter)) {
    if (!desc.setWritable) {
      desc.writable = false;
    }
  }
  if (!desc.setEnumerable) {
    desc.enumerable = false;
  }
  if (!desc.setConfigurable) {
    desc.configurable = false;
  }
}

// ES9 9.1.6.2 IsCompatiblePropertyDescriptor
// prereq: step 2 is already done externally.
// The abstract definition returns a boolean; this returns EXCEPTION
// (and sets an exception) or RETURNED instead of false or true, so
// the exception messages can be more specific.
ExecutionStatus isCompatiblePropertyDescriptor(
    Runtime &runtime,
    const DefinePropertyFlags &desc,
    Handle<> descValueOrAccessor,
    const ComputedPropertyDescriptor &current,
    Handle<> currentValueOrAccessor) {
  // 4. If current.[[Configurable]] is false, then
  if (!current.flags.configurable) {
    // a. If Desc.[[Configurable]] is present and its value is true, return
    // false.
    if (desc.setConfigurable && desc.configurable) {
      return runtime.raiseTypeError(
          "trap result is configurable but target property is non-configurable");
    }
    // b. If Desc.[[Enumerable]] is present and the [[Enumerable]]
    // fields of current and Desc are the Boolean negation of each
    // other, return false.
    if (desc.setEnumerable && desc.enumerable != current.flags.enumerable) {
      return runtime.raiseTypeError(
          TwineChar16("trap result is ") + (desc.enumerable ? "" : "not ") +
          "enumerable but target property is " +
          (current.flags.enumerable ? "" : "not ") + "enumerable");
    }
  }

  // 5. If IsGenericDescriptor(Desc) is true, no further validation is required.
  bool descIsAccessor = desc.setSetter || desc.setGetter;
  bool descIsData = desc.setValue || desc.setWritable;
  assert(
      (!descIsData || !descIsAccessor) &&
      "descriptor cannot be both Data and Accessor");
  if (!descIsData && !descIsAccessor) {
    return ExecutionStatus::RETURNED;
  }
  // 6. Else if IsDataDescriptor(current) and IsDataDescriptor(Desc)
  // have different results, then
  bool currentIsAccessor = current.flags.accessor;
  bool currentIsData = !currentIsAccessor;
  if (currentIsData != descIsData) {
    // a. If current.[[Configurable]] is false, return false.
    if (!current.flags.configurable) {
      return runtime.raiseTypeError(
          TwineChar16("trap result is ") +
          (currentIsData ? "data " : "accessor ") + "but target property is " +
          (descIsData ? "data " : "accessor ") + "and non-configurable");
    }
  }
  // 7. Else if IsDataDescriptor(current) and IsDataDescriptor(Desc) are both
  // true, then
  //   a. If current.[[Configurable]] is false and current.[[Writable]] is
  //   false, then
  if (currentIsData && descIsData && !current.flags.configurable &&
      !current.flags.writable) {
    // i. If Desc.[[Writable]] is present and Desc.[[Writable]] is true,
    // return false.
    if (desc.setWritable && desc.writable) {
      return runtime.raiseTypeError(
          "trap result is writable but "
          "target property is non-configurable and non-writable");
    }
    // ii. If Desc.[[Value]] is present and SameValue(Desc.[[Value]],
    // current.[[Value]]) is false, return false.
    if (desc.setValue &&
        !isSameValue(
            descValueOrAccessor.getHermesValue(),
            currentValueOrAccessor.getHermesValue())) {
      return runtime.raiseTypeError(
          "trap result has different value than target property but "
          "target property is non-configurable and non-writable");
    }
    // iii. Return true.
    return ExecutionStatus::RETURNED;
  }
  // 8. Else IsAccessorDescriptor(current) and IsAccessorDescriptor(Desc) are
  // both true,
  //   a. If current.[[Configurable]] is false, then
  if (currentIsAccessor && descIsAccessor && !current.flags.configurable) {
    PropertyAccessor *descAccessor =
        vmcast<PropertyAccessor>(descValueOrAccessor.get());
    PropertyAccessor *currentAccessor =
        vmcast<PropertyAccessor>(currentValueOrAccessor.get());
    // i. If Desc.[[Set]] is present and SameValue(Desc.[[Set]],
    // current.[[Set]]) is false, return false.
    if (descAccessor->setter &&
        descAccessor->setter != currentAccessor->setter) {
      return runtime.raiseTypeError(
          "trap result has different setter than target property but "
          "target property is non-configurable");
    }
    // ii. If Desc.[[Get]] is present and SameValue(Desc.[[Get]],
    // current.[[Get]]) is false, return false.
    if (descAccessor->getter &&
        descAccessor->getter != currentAccessor->getter) {
      return runtime.raiseTypeError(
          "trap result has different getter than target property but "
          "target property is non-configurable");
    }
    // iii. Return true.
  }
  return ExecutionStatus::RETURNED;
}

} // namespace

CallResult<PseudoHandle<JSObject>> JSProxy::getPrototypeOf(
    Handle<JSObject> selfHandle,
    Runtime &runtime) {
  // Proxies are complex, and various parts of the logic (finding
  // traps, undefined traps handling, calling traps, etc) are all
  // potentially recursive.  Therefore, every entry point creates a
  // scope and a ScopedNativeDepthTracker, as it's possible to use up
  // arbitrary native stack depth with nested proxies.
  GCScope gcScope(runtime);
  ScopedNativeDepthTracker depthTracker(runtime);
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }
  // Make sure to retrieve the target and handler before any JS can execute.
  auto &slots = detail::slots(*selfHandle);
  Handle<JSObject> target = runtime.makeHandle(slots.target);
  Handle<JSObject> handler = runtime.makeHandle(slots.handler);
  CallResult<Handle<Callable>> trapRes =
      detail::findTrap(selfHandle, runtime, Predefined::getPrototypeOf);
  if (LLVM_UNLIKELY(trapRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 6. If trap is undefined, then
  if (!*trapRes) {
    //   a. Return ? target.[[GetPrototypeOf]](P).
    return JSObject::getPrototypeOf(target, runtime);
  }
  // 7. Let handlerProto be ? Call(trap, handler, « target »).
  CallResult<PseudoHandle<>> handlerProtoRes = Callable::executeCall1(
      *trapRes, runtime, handler, target.getHermesValue());
  if (handlerProtoRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 8. If Type(handlerProto) is neither Object nor Null, throw a TypeError
  // exception.
  if (!(*handlerProtoRes)->isObject() && !(*handlerProtoRes)->isNull()) {
    return runtime.raiseTypeError(
        "getPrototypeOf trap result is neither Object nor Null");
  }
  Handle<JSObject> handlerProto =
      runtime.makeHandle(dyn_vmcast<JSObject>(handlerProtoRes->get()));

  // 9. Let extensibleTarget be ? IsExtensible(target).
  CallResult<bool> extensibleRes = JSObject::isExtensible(target, runtime);
  if (extensibleRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 10. If extensibleTarget is true, return handlerProto.
  if (*extensibleRes) {
    return createPseudoHandle(*handlerProto);
  }
  // 11. Let targetProto be ? target.[[GetPrototypeOf]]().
  CallResult<PseudoHandle<JSObject>> targetProtoRes =
      JSObject::getPrototypeOf(target, runtime);
  if (targetProtoRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 12. If SameValue(handlerProto, targetProto) is false, throw a TypeError
  // exception.
  if (handlerProto.get() != targetProtoRes->get()) {
    return runtime.raiseTypeError(
        "getPrototypeOf trap result is not the same as non-extensible target getPrototypeOf");
  }
  // 13. Return handlerProto.
  return std::move(*targetProtoRes);
}

CallResult<bool> JSProxy::setPrototypeOf(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<JSObject> parent) {
  GCScope gcScope{runtime};
  ScopedNativeDepthTracker depthTracker(runtime);
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }
  // Make sure to retrieve the target and handler before any JS can execute.
  auto &slots = detail::slots(*selfHandle);
  Handle<JSObject> target = runtime.makeHandle(slots.target);
  Handle<JSObject> handler = runtime.makeHandle(slots.handler);
  CallResult<Handle<Callable>> trapRes =
      detail::findTrap(selfHandle, runtime, Predefined::setPrototypeOf);
  if (trapRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 7. If trap is undefined, then
  if (!*trapRes) {
    //   a. Return ? target.[[SetPrototypeOf]](V).
    return JSObject::setParent(*target, runtime, *parent);
  }
  // 8. Let booleanTrapResult be ToBoolean(? Call(trap, handler, « target, V
  // »)).
  CallResult<PseudoHandle<>> booleanTrapRes = Callable::executeCall2(
      *trapRes,
      runtime,
      handler,
      target.getHermesValue(),
      *parent ? parent.getHermesValue() : HermesValue::encodeNullValue());
  if (booleanTrapRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 9. If booleanTrapResult is false, return false.
  if (!toBoolean(booleanTrapRes->get())) {
    return false;
  }
  // 10. Let extensibleTarget be ? IsExtensible(target).
  CallResult<bool> extensibleRes = JSObject::isExtensible(target, runtime);
  if (extensibleRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 11. If extensibleTarget is true, return true.
  if (*extensibleRes) {
    return true;
  }
  // 12. Let targetProto be ? target.[[GetPrototypeOf]]().
  CallResult<PseudoHandle<JSObject>> targetProtoRes =
      JSObject::getPrototypeOf(target, runtime);
  if (targetProtoRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 13. If SameValue(V, targetProto) is false, throw a TypeError exception.
  if (parent.get() != targetProtoRes->get()) {
    return runtime.raiseTypeError(
        "setPrototypeOf trap changed prototype on non-extensible target");
  }
  // 14. Return true.
  return true;
}

CallResult<bool> JSProxy::isExtensible(
    Handle<JSObject> selfHandle,
    Runtime &runtime) {
  GCScope gcScope{runtime};
  ScopedNativeDepthTracker depthTracker(runtime);
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }
  // Make sure to retrieve the target and handler before any JS can execute.
  auto &slots = detail::slots(*selfHandle);
  Handle<JSObject> target = runtime.makeHandle(slots.target);
  Handle<JSObject> handler = runtime.makeHandle(slots.handler);
  CallResult<Handle<Callable>> trapRes =
      detail::findTrap(selfHandle, runtime, Predefined::isExtensible);
  if (trapRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 6. If trap is undefined, then
  if (!*trapRes) {
    //   a. Return ? target.[[IsExtensible]]().
    return JSObject::isExtensible(target, runtime);
  }
  // 7. Let booleanTrapResult be ToBoolean(? Call(trap, handler, « target »)).
  CallResult<PseudoHandle<>> res = Callable::executeCall1(
      *trapRes, runtime, handler, target.getHermesValue());
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  bool booleanTrapResult = toBoolean(res->get());
  res->invalidate();
  // 8. Let targetResult be ? target.[[IsExtensible]]().
  CallResult<bool> targetRes = JSObject::isExtensible(target, runtime);
  if (targetRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 9. If SameValue(booleanTrapResult, targetResult) is false, throw
  //    a TypeError exception.
  if (booleanTrapResult != *targetRes) {
    return runtime.raiseTypeError(
        "isExtensible trap returned different value than target");
  }
  // 10. Return booleanTrapResult.
  return booleanTrapResult;
}

CallResult<bool> JSProxy::preventExtensions(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    PropOpFlags opFlags) {
  GCScope gcScope{runtime};
  ScopedNativeDepthTracker depthTracker(runtime);
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }
  // Make sure to retrieve the target and handler before any JS can execute.
  auto &slots = detail::slots(*selfHandle);
  Handle<JSObject> target = runtime.makeHandle(slots.target);
  Handle<JSObject> handler = runtime.makeHandle(slots.handler);
  CallResult<Handle<Callable>> trapRes =
      detail::findTrap(selfHandle, runtime, Predefined::preventExtensions);
  if (trapRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 6. If trap is undefined, then
  if (!*trapRes) {
    //   a. Return ? target.[[PreventExtensions]]().
    // We pass in opFlags here.  If getThrowOnError, then this will cause
    // the underlying exception to bubble up.  If !getThrowOnError, then
    // we don't get a chance to raise a particular exception anyway.  So in
    // either case, just return the CallResult.
    return JSObject::preventExtensions(target, runtime, opFlags);
  }
  // 7. Let booleanTrapResult be ToBoolean(? Call(trap, handler, « target »)).
  CallResult<PseudoHandle<>> res = Callable::executeCall1(
      *trapRes, runtime, handler, target.getHermesValue());
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  bool booleanTrapResult = toBoolean(res->get());
  if (booleanTrapResult) {
    // a. Let targetIsExtensible be ? target.[[IsExtensible]]().
    CallResult<bool> targetRes = JSObject::isExtensible(target, runtime);
    if (targetRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    // b. If targetIsExtensible is true, throw a TypeError exception.
    if (*targetRes) {
      return runtime.raiseTypeError(
          "preventExtensions trap returned true for extensible target");
    }
  }
  // 10. Return booleanTrapResult.
  if (!booleanTrapResult && opFlags.getThrowOnError()) {
    return runtime.raiseTypeError("preventExtensions trap returned false");
  }
  return booleanTrapResult;
}

CallResult<bool> JSProxy::getOwnProperty(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<> nameValHandle,
    ComputedPropertyDescriptor &desc,
    MutableHandle<> *valueOrAccessor) {
  GCScope gcScope{runtime};
  ScopedNativeDepthTracker depthTracker(runtime);
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }
  // Make sure to retrieve the target and handler before any JS can execute.
  auto &slots = detail::slots(*selfHandle);
  Handle<JSObject> target = runtime.makeHandle(slots.target);
  Handle<JSObject> handler = runtime.makeHandle(slots.handler);
  CallResult<Handle<Callable>> trapRes = detail::findTrap(
      selfHandle, runtime, Predefined::getOwnPropertyDescriptor);
  if (trapRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 7. If trap is undefined, then
  if (!*trapRes) {
    //   a. Return ? target.[[GetOwnProperty]](P).
    return valueOrAccessor
        ? JSObject::getOwnComputedDescriptor(
              target, runtime, nameValHandle, desc, *valueOrAccessor)
        : JSObject::getOwnComputedDescriptor(
              target, runtime, nameValHandle, desc);
  }
  // 8. Let trapResultObj be ? Call(trap, handler, « target, P »).
  // 9. If Type(trapResultObj) is neither Object nor Undefined, throw a
  // TypeError exception.
  CallResult<PseudoHandle<>> trapResultRes = Callable::executeCall2(
      *trapRes,
      runtime,
      handler,
      target.getHermesValue(),
      nameValHandle.getHermesValue());
  if (trapResultRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<> trapResultObj = runtime.makeHandle(std::move(*trapResultRes));
  // 10. Let targetDesc be ? target.[[GetOwnProperty]](P).
  ComputedPropertyDescriptor targetDesc;
  MutableHandle<> targetValueOrAccessor{runtime};
  CallResult<bool> targetDescRes = JSObject::getOwnComputedDescriptor(
      target, runtime, nameValHandle, targetDesc, targetValueOrAccessor);
  if (targetDescRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 11. If trapResultObj is undefined, then
  if (trapResultObj->isUndefined()) {
    //   a. If targetDesc is undefined, return undefined.
    if (!*targetDescRes) {
      return false;
    }
    //   b. If targetDesc.[[Configurable]] is false, throw a TypeError
    //   exception.
    if (!targetDesc.flags.configurable) {
      return runtime.raiseTypeError(
          "getOwnPropertyDescriptor trap result is not configurable");
    }
    //   c. Let extensibleTarget be ? IsExtensible(target).
    CallResult<bool> extensibleRes = JSObject::isExtensible(target, runtime);
    if (extensibleRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    //   d. Assert: Type(extensibleTarget) is Boolean.
    //   e. If extensibleTarget is false, throw a TypeError exception.
    if (!*extensibleRes) {
      return runtime.raiseTypeErrorForValue(
          target, " is not extensible (getOwnPropertyDescriptor target)");
    }
    //   f. Return undefined.
    return false;
  } else if (!trapResultObj->isObject()) {
    // 9. If Type(trapResultObj) is neither Object nor Undefined, throw a
    // TypeError exception.
    return runtime.raiseTypeErrorForValue(
        trapResultObj,
        " is not undefined or Object (Proxy getOwnPropertyDescriptor)");
  }
  // 12. Let extensibleTarget be ? IsExtensible(target).
  CallResult<bool> extensibleRes = JSObject::isExtensible(target, runtime);
  if (extensibleRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 13. Let resultDesc be ? ToPropertyDescriptor(trapResultObj).
  // 14. Call CompletePropertyDescriptor(resultDesc).
  DefinePropertyFlags resultDesc;
  MutableHandle<> resultValueOrAccessor{runtime};
  Handle<JSObject> trapResult = runtime.makeHandle<JSObject>(*trapResultObj);
  if (LLVM_UNLIKELY(
          toPropertyDescriptor(
              trapResult, runtime, resultDesc, resultValueOrAccessor) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  completePropertyDescriptor(resultDesc);
  // 15. Let valid be IsCompatiblePropertyDescriptor(extensibleTarget,
  // resultDesc, targetDesc).
  // 16. If valid is false, throw a TypeError exception.

  // ES9 9.1.6.3 ValidateAndApplyPropertyDescriptor step 2 [O is undefined]
  if (!*targetDescRes) {
    // a. If extensible is false, return false.
    if (!*extensibleRes) {
      return runtime.raiseTypeErrorForValue(
          "getOwnPropertyDescriptor target is not extensible and has no property ",
          nameValHandle,
          "");
    }
    // e. return true
    // this concludes steps 15 and 16.
  } else {
    if (LLVM_UNLIKELY(
            isCompatiblePropertyDescriptor(
                runtime,
                resultDesc,
                resultValueOrAccessor,
                targetDesc,
                targetValueOrAccessor) == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  // 17. If resultDesc.[[Configurable]] is false, then
  if (!resultDesc.configurable) {
    //   a. If targetDesc is undefined or targetDesc.[[Configurable]] is true,
    //   then
    //     i. Throw a TypeError exception.
    if (!*targetDescRes || targetDesc.flags.configurable) {
      return runtime.raiseTypeErrorForValue(
          "getOwnPropertyDescriptor trap result is not configurable but "
          "target property ",
          nameValHandle,
          " is configurable or non-existent");
    }
    //   b. If resultDesc has a [[Writable]] field and
    //   resultDesc.[[Writable]] is false, then
    if (resultDesc.setWritable && !resultDesc.writable) {
      //     i. Assert: targetDesc has a [[Writable]] field.
      assert(
          *targetDescRes && !targetDesc.flags.accessor &&
          "targetDesc must be a data descriptor with a [[Writable]] field");
      //     ii. If targetDesc.[[Writable]] is true, throw a TypeError
      //     exception.
      if (targetDesc.flags.writable) {
        return runtime.raiseTypeErrorForValue(
            "getOwnPropertyDescriptor trap result is non-configurable and "
            "non-writable but target property ",
            nameValHandle,
            " is writable");
      }
    }
  }
  // 18. Return resultDesc.
  desc.flags.enumerable = resultDesc.enumerable;
  desc.flags.configurable = resultDesc.configurable;
  desc.flags.writable = resultDesc.writable;
  if (resultDesc.setGetter || resultDesc.setSetter) {
    desc.flags.accessor = true;
  }
  if (valueOrAccessor) {
    *valueOrAccessor = std::move(resultValueOrAccessor);
  }
  return true;
}

CallResult<bool> JSProxy::defineOwnProperty(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<> nameValHandle,
    DefinePropertyFlags dpFlags,
    Handle<> valueOrAccessor,
    PropOpFlags opFlags) {
  GCScope gcScope{runtime};
  ScopedNativeDepthTracker depthTracker(runtime);
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }
  // Make sure to retrieve the target and handler before any JS can execute.
  auto &slots = detail::slots(*selfHandle);
  Handle<JSObject> target = runtime.makeHandle(slots.target);
  Handle<JSObject> handler = runtime.makeHandle(slots.handler);
  CallResult<Handle<Callable>> trapRes =
      detail::findTrap(selfHandle, runtime, Predefined::defineProperty);
  if (trapRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 7. If trap is undefined, then
  if (!*trapRes) {
    //   a. Return ? target.[[GetOwnProperty]](P).
    return JSObject::defineOwnComputedPrimitive(
        target, runtime, nameValHandle, dpFlags, valueOrAccessor, opFlags);
  }
  // 8. Let descObj be FromPropertyDescriptor(Desc).
  CallResult<HermesValue> descObjRes =
      objectFromPropertyDescriptor(runtime, dpFlags, valueOrAccessor);
  if (descObjRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 9. Let booleanTrapResult be ToBoolean(? Call(trap, handler, « target, P,
  // descObj »)).
  CallResult<PseudoHandle<>> trapResultRes = Callable::executeCall3(
      *trapRes,
      runtime,
      handler,
      target.getHermesValue(),
      nameValHandle.getHermesValue(),
      *descObjRes);
  if (trapResultRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  bool trapResult = toBoolean(trapResultRes->get());
  // 10. If booleanTrapResult is false, return false.
  if (!trapResult) {
    if (opFlags.getThrowOnError()) {
      return runtime.raiseTypeError("defineProperty proxy trap returned false");
    } else {
      return false;
    }
  }
  // 11. Let targetDesc be ? target.[[GetOwnProperty]](P).
  ComputedPropertyDescriptor targetDesc;
  MutableHandle<> targetDescValueOrAccessor{runtime};
  CallResult<bool> targetDescRes = JSObject::getOwnComputedDescriptor(
      target, runtime, nameValHandle, targetDesc, targetDescValueOrAccessor);
  if (targetDescRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 12. Let extensibleTarget be ? IsExtensible(target).
  CallResult<bool> extensibleRes = JSObject::isExtensible(target, runtime);
  if (extensibleRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 13. If Desc has a [[Configurable]] field and if Desc.[[Configurable]] is
  // false, then
  //   a. Let settingConfigFalse be true.
  // 14. Else, let settingConfigFalse be false.
  bool settingConfigFalse = dpFlags.setConfigurable && !dpFlags.configurable;
  // 15. If targetDesc is undefined, then
  if (!*targetDescRes) {
    //   a. If extensibleTarget is false, throw a TypeError exception.
    if (!*extensibleRes) {
      return runtime.raiseTypeError(
          "defineProperty trap called for non-existent property on non-extensible target");
    }
    //   b. If settingConfigFalse is true, throw a TypeError exception.
    if (settingConfigFalse) {
      return runtime.raiseTypeError(
          "defineProperty trap attempted to define non-configurable property for non-existent property in the target");
    }
  } else {
    // 16. Else targetDesc is not undefined,
    //   a. If IsCompatiblePropertyDescriptor(extensibleTarget, Desc,
    //   targetDesc) is false, throw a TypeError exception.
    if (LLVM_UNLIKELY(
            isCompatiblePropertyDescriptor(
                runtime,
                dpFlags,
                valueOrAccessor,
                targetDesc,
                targetDescValueOrAccessor) == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    //   b. If settingConfigFalse is true and targetDesc.[[Configurable]] is
    //   true, throw a TypeError exception.
    if (settingConfigFalse && targetDesc.flags.configurable) {
      return runtime.raiseTypeError(
          "defineProperty trap attempted to define non-configurable property for configurable property in the target");
    }
    //   c. If IsDataDescriptor(targetDesc) is true,
    //   targetDesc.[[Configurable]] is false, and targetDesc.[[Writable]] is
    //   true, then
    //     i. If Desc has a [[Writable]] field and Desc.[[Writable]] is false,
    //     throw a TypeError exception.
    if (!targetDesc.flags.accessor && !targetDesc.flags.configurable &&
        targetDesc.flags.writable && dpFlags.setWritable && !dpFlags.writable) {
      return runtime.raiseTypeError(
          "defineProperty trap attempted to define non-writable property for writable property in the target");
    }
  }
  // 17. Return true.
  return true;
}

namespace {

/// Common parts of hasNamed/hasComputed
CallResult<bool> hasWithTrap(
    Runtime &runtime,
    Handle<> nameValHandle,
    Handle<Callable> trap,
    Handle<JSObject> handler,
    Handle<JSObject> target) {
  // 1. Assert: IsPropertyKey(P) is true.
  assert(isPropertyKey(nameValHandle) && "key is not a String or Symbol");
  // 8. Let booleanTrapResult be ToBoolean(? Call(trap, handler, « target, P
  // »)).
  CallResult<PseudoHandle<>> trapResultRes = Callable::executeCall2(
      trap,
      runtime,
      handler,
      target.getHermesValue(),
      nameValHandle.getHermesValue());
  if (trapResultRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  bool trapResult = toBoolean(trapResultRes->get());
  // 9. If booleanTrapResult is false, then
  if (!trapResult) {
    //   a. Let targetDesc be ? target.[[GetOwnProperty]](P).
    ComputedPropertyDescriptor targetDesc;
    CallResult<bool> targetDescRes = JSObject::getOwnComputedDescriptor(
        target, runtime, nameValHandle, targetDesc);
    if (targetDescRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    //   b. If targetDesc is not undefined, then
    if (*targetDescRes) {
      //     i. If targetDesc.[[Configurable]] is false, throw a TypeError
      //     exception.
      if (!targetDesc.flags.configurable) {
        return runtime.raiseTypeError(
            "HasProperty trap result is not configurable");
      }
      //     ii. Let extensibleTarget be ? IsExtensible(target).
      CallResult<bool> extensibleRes = JSObject::isExtensible(target, runtime);
      if (extensibleRes == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      //     iii. If extensibleTarget is false, throw a TypeError exception.
      if (!*extensibleRes) {
        return runtime.raiseTypeError(
            "HasProperty proxy target is not extensible");
      }
    }
  }
  // 11. Return trapResult.
  return trapResult;
}

} // namespace

CallResult<bool> JSProxy::hasNamed(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SymbolID name) {
  GCScope gcScope{runtime};
  ScopedNativeDepthTracker depthTracker(runtime);
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }
  // Make sure to retrieve the target and handler before any JS can execute.
  auto &slots = detail::slots(*selfHandle);
  Handle<JSObject> target = runtime.makeHandle(slots.target);
  Handle<JSObject> handler = runtime.makeHandle(slots.handler);
  CallResult<Handle<Callable>> trapRes =
      detail::findTrap(selfHandle, runtime, Predefined::has);
  if (trapRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 7. If trap is undefined, then
  if (!*trapRes) {
    //   a. Return ? target.[[HasProperty]](P, Receiver).
    return JSObject::hasNamed(target, runtime, name);
  }
  return hasWithTrap(
      runtime,
      runtime.makeHandle(
          HermesValue::encodeStringValue(
              runtime.getStringPrimFromSymbolID(name))),
      *trapRes,
      handler,
      target);
}

CallResult<bool> JSProxy::hasComputed(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<> nameValHandle) {
  GCScope gcScope{runtime};
  ScopedNativeDepthTracker depthTracker(runtime);
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }
  // Make sure to retrieve the target and handler before any JS can execute.
  auto &slots = detail::slots(*selfHandle);
  Handle<JSObject> target = runtime.makeHandle(slots.target);
  Handle<JSObject> handler = runtime.makeHandle(slots.handler);
  CallResult<Handle<Callable>> trapRes =
      detail::findTrap(selfHandle, runtime, Predefined::has);
  if (trapRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  if (!*trapRes) {
    // 7. If trap is undefined, then
    //   a. Return ? target.[[HasProperty]](P, Receiver).
    return JSObject::hasComputed(target, runtime, nameValHandle);
  }
  return hasWithTrap(runtime, nameValHandle, *trapRes, handler, target);
}

namespace {

/// Common parts of getNamed/getComputed
CallResult<PseudoHandle<>> getWithTrap(
    Runtime &runtime,
    Handle<> nameValHandle,
    Handle<Callable> trap,
    Handle<JSObject> handler,
    Handle<JSObject> target,
    Handle<> receiver) {
  // 1. Assert: IsPropertyKey(P) is true.
  assert(isPropertyKey(nameValHandle) && "key is not a String or Symbol");
  // 8. Let trapResult be ? Call(trap, handler, « target, P, Receiver »).
  CallResult<PseudoHandle<>> trapResultRes = Callable::executeCall3(
      trap,
      runtime,
      handler,
      target.getHermesValue(),
      nameValHandle.getHermesValue(),
      receiver.getHermesValue());
  if (trapResultRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<> trapResult = runtime.makeHandle(std::move(*trapResultRes));
  // 9. Let targetDesc be ? target.[[GetOwnProperty]](P).
  ComputedPropertyDescriptor targetDesc;
  MutableHandle<> targetValueOrAccessor{runtime};
  CallResult<bool> targetDescRes = JSObject::getOwnComputedDescriptor(
      target, runtime, nameValHandle, targetDesc, targetValueOrAccessor);
  if (targetDescRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 10. If targetDesc is not undefined and targetDesc.[[Configurable]] is
  // false, then
  if (*targetDescRes && !targetDesc.flags.configurable) {
    //   a. If IsDataDescriptor(targetDesc) is true and targetDesc.[[Writable]]
    //   is false, then
    if (!targetDesc.flags.accessor && !targetDesc.flags.writable) {
      //     i. If SameValue(trapResult, targetDesc.[[Value]]) is false, throw a
      //     TypeError exception.
      if (!isSameValue(*trapResult, targetValueOrAccessor.getHermesValue())) {
        return runtime.raiseTypeError(
            "target property is non-configurable and non-writable, and get trap result differs from target property value");
      }
    }
    //   b. If IsAccessorDescriptor(targetDesc) is true and targetDesc.[[Get]]
    //   is undefined, then
    //     i. If trapResult is not undefined, throw a TypeError exception.
    if (targetDesc.flags.accessor &&
        !vmcast<PropertyAccessor>(*targetValueOrAccessor)->getter &&
        !trapResult->isUndefined()) {
      return runtime.raiseTypeError(
          "target property is non-configurable accessor with no getter, but get trap returned not undefined");
    }
  }

  // 11. Return trapResult.
  return {trapResult};
}

} // namespace

CallResult<PseudoHandle<>> JSProxy::getNamed(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SymbolID name,
    Handle<> receiver) {
  GCScope gcScope{runtime};
  ScopedNativeDepthTracker depthTracker(runtime);
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }
  // Make sure to retrieve the target and handler before calling findTrap, as
  // that may result in these fields being erased if the proxy is revoked in the
  // handler.
  auto &slots = detail::slots(*selfHandle);
  Handle<JSObject> target = runtime.makeHandle(slots.target);
  Handle<JSObject> handler = runtime.makeHandle(slots.handler);
  CallResult<Handle<Callable>> trapRes =
      detail::findTrap(selfHandle, runtime, Predefined::get);
  if (trapRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 7. If trap is undefined, then
  if (!*trapRes) {
    //   a. Return ? target.[[Get]](P, Receiver).
    return JSObject::getNamedWithReceiver_RJS(target, runtime, name, receiver);
  }
  return getWithTrap(
      runtime,
      name.isUniqued() ? runtime.makeHandle(
                             HermesValue::encodeStringValue(
                                 runtime.getStringPrimFromSymbolID(name)))
                       : runtime.makeHandle(name),
      *trapRes,
      handler,
      target,
      receiver);
}

CallResult<PseudoHandle<>> JSProxy::getComputed(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<> nameValHandle,
    Handle<> receiver) {
  GCScope gcScope{runtime};
  ScopedNativeDepthTracker depthTracker(runtime);
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }
  // Make sure to retrieve the target and handler before any JS can execute.
  auto &slots = detail::slots(*selfHandle);
  Handle<JSObject> target = runtime.makeHandle(slots.target);
  Handle<JSObject> handler = runtime.makeHandle(slots.handler);
  CallResult<Handle<Callable>> trapRes =
      detail::findTrap(selfHandle, runtime, Predefined::get);
  if (trapRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 7. If trap is undefined, then
  if (!*trapRes) {
    //   a. Return ? target.[[Get]](P, Receiver).
    return JSObject::getComputedWithReceiver_RJS(
        target, runtime, nameValHandle, receiver);
  }
  return getWithTrap(
      runtime, nameValHandle, *trapRes, handler, target, receiver);
}

namespace {

/// Common parts of setNamed/setComputed
CallResult<bool> setWithTrap(
    Runtime &runtime,
    Handle<> nameValHandle,
    Handle<> valueHandle,
    Handle<Callable> trap,
    Handle<JSObject> handler,
    Handle<JSObject> target,
    Handle<> receiver) {
  // 1. Assert: IsPropertyKey(P) is true.
  assert(isPropertyKey(nameValHandle) && "key is not a String or Symbol");
  // 8. Let booleanTrapResult be ToBoolean(? Call(trap, handler, « target, P, V,
  // Receiver »)).
  CallResult<PseudoHandle<>> trapResultRes = Callable::executeCall4(
      trap,
      runtime,
      handler,
      target.getHermesValue(),
      nameValHandle.getHermesValue(),
      valueHandle.getHermesValue(),
      receiver.getHermesValue());
  if (trapResultRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 9. If booleanTrapResult is false, return false.
  if (!toBoolean(trapResultRes->get())) {
    return false;
  }
  // 10. Let targetDesc be ? target.[[GetOwnProperty]](P).
  ComputedPropertyDescriptor targetDesc;
  MutableHandle<> targetValueOrAccessor{runtime};
  CallResult<bool> targetDescRes = JSObject::getOwnComputedDescriptor(
      target, runtime, nameValHandle, targetDesc, targetValueOrAccessor);
  if (targetDescRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 11. If targetDesc is not undefined and targetDesc.[[Configurable]] is
  // false, then
  if (*targetDescRes && !targetDesc.flags.configurable) {
    //   a. If IsDataDescriptor(targetDesc) is true and targetDesc.[[Writable]]
    //   is false, then
    if (!targetDesc.flags.accessor && !targetDesc.flags.writable) {
      //     i. If SameValue(V, targetDesc.[[Value]]) is false, throw a
      //     TypeError exception.
      if (!isSameValue(
              valueHandle.getHermesValue(),
              targetValueOrAccessor.getHermesValue())) {
        return runtime.raiseTypeError(
            "target property is non-configurable and non-writable, and set trap value differs from target property value");
      }
    }
    //   b. If IsAccessorDescriptor(targetDesc) is true, then
    //     i. If targetDesc.[[Set]] is undefined, throw a TypeError exception.
    if (targetDesc.flags.accessor &&
        !vmcast<PropertyAccessor>(*targetValueOrAccessor)->setter) {
      return runtime.raiseTypeError(
          "set trap called, but target property is non-configurable accessor with no setter");
    }
  }

  // 12. Return true.
  return true;
}

} // namespace

CallResult<bool> JSProxy::setNamed(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SymbolID name,
    Handle<> valueHandle,
    // TODO could be HermesValue
    Handle<> receiver) {
  GCScope gcScope{runtime};
  ScopedNativeDepthTracker depthTracker(runtime);
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }
  // Make sure to retrieve the target and handler before any JS can execute.
  auto &slots = detail::slots(*selfHandle);
  Handle<JSObject> target = runtime.makeHandle(slots.target);
  Handle<JSObject> handler = runtime.makeHandle(slots.handler);
  CallResult<Handle<Callable>> trapRes =
      detail::findTrap(selfHandle, runtime, Predefined::set);
  if (trapRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 7. If trap is undefined, then
  if (!*trapRes) {
    //   a. Return ? target.[[Set]](P, V, Receiver).
    return JSObject::putNamedWithReceiver_RJS(
        target, runtime, name, valueHandle, receiver);
  }
  return setWithTrap(
      runtime,
      name.isUniqued() ? runtime.makeHandle(
                             HermesValue::encodeStringValue(
                                 runtime.getStringPrimFromSymbolID(name)))
                       : runtime.makeHandle(name),
      valueHandle,
      *trapRes,
      handler,
      target,
      receiver);
}

CallResult<bool> JSProxy::setComputed(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<> nameValHandle,
    Handle<> valueHandle,
    // TODO could be HermesValue
    Handle<> receiver) {
  GCScope gcScope{runtime};
  ScopedNativeDepthTracker depthTracker(runtime);
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }
  // Make sure to retrieve the target and handler before any JS can execute.
  auto &slots = detail::slots(*selfHandle);
  Handle<JSObject> target = runtime.makeHandle(slots.target);
  Handle<JSObject> handler = runtime.makeHandle(slots.handler);
  CallResult<Handle<Callable>> trapRes =
      detail::findTrap(selfHandle, runtime, Predefined::set);
  if (trapRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 7. If trap is undefined, then
  if (!*trapRes) {
    //   a. Return ? target.[[Set]](P, V, Receiver).
    return JSObject::putComputedWithReceiver_RJS(
        target, runtime, nameValHandle, valueHandle, receiver);
  }
  return setWithTrap(
      runtime, nameValHandle, valueHandle, *trapRes, handler, target, receiver);
}

namespace {

/// Common parts of deleteNamed/deleteComputed
CallResult<bool> deleteWithTrap(
    Runtime &runtime,
    Handle<> nameValHandle,
    Handle<Callable> trap,
    Handle<JSObject> handler,
    Handle<JSObject> target) {
  // 1. Assert: IsPropertyKey(P) is true.
  assert(isPropertyKey(nameValHandle) && "key is not a String or Symbol");
  // 8. Let booleanTrapResult be ToBoolean(? Call(trap, handler, « target, P
  // »)).
  CallResult<PseudoHandle<>> trapResultRes = Callable::executeCall2(
      trap,
      runtime,
      handler,
      target.getHermesValue(),
      nameValHandle.getHermesValue());
  if (trapResultRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  bool trapResult = toBoolean(trapResultRes->getHermesValue());
  // 9. If booleanTrapResult is false, return false.
  if (!trapResult) {
    return false;
  }
  // 10. Let targetDesc be ? target.[[GetOwnProperty]](P).
  ComputedPropertyDescriptor targetDesc;
  MutableHandle<> targetValueOrAccessor{runtime};
  CallResult<bool> targetDescRes = JSObject::getOwnComputedDescriptor(
      target, runtime, nameValHandle, targetDesc, targetValueOrAccessor);
  if (targetDescRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 11. If targetDesc is undefined, return true.
  if (!*targetDescRes) {
    return true;
  }
  // 12. If targetDesc.[[Configurable]] is false, throw a TypeError exception.
  if (!targetDesc.flags.configurable) {
    return runtime.raiseTypeError(
        "Delete trap target called, but target property is non-configurable");
  }
  // 13. Let extensibleTarget be ? IsExtensible(target).
  CallResult<bool> extensibleRes = JSObject::isExtensible(target, runtime);
  if (LLVM_UNLIKELY(extensibleRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 14. If extensibleTarget is false, throw a TypeError exception.
  if (!*extensibleRes) {
    return runtime.raiseTypeError(
        "Delete trap returned true for configurable property on non-extensible target");
  }
  // 15. Return true.
  return true;
}

} // namespace

CallResult<bool> JSProxy::deleteNamed(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    SymbolID name) {
  GCScope gcScope{runtime};
  ScopedNativeDepthTracker depthTracker(runtime);
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }
  // Make sure to retrieve the target and handler before any JS can execute.
  auto &slots = detail::slots(*selfHandle);
  Handle<JSObject> target = runtime.makeHandle(slots.target);
  Handle<JSObject> handler = runtime.makeHandle(slots.handler);
  CallResult<Handle<Callable>> trapRes =
      detail::findTrap(selfHandle, runtime, Predefined::deleteProperty);
  if (trapRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 7. If trap is undefined, then
  if (!*trapRes) {
    //   a. Return ? target.[[Delete]](P, Receiver).
    return JSObject::deleteNamed(target, runtime, name);
  }
  return deleteWithTrap(
      runtime,
      runtime.makeHandle(
          HermesValue::encodeStringValue(
              runtime.getStringPrimFromSymbolID(name))),
      *trapRes,
      handler,
      target);
}

CallResult<bool> JSProxy::deleteComputed(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    Handle<> nameValHandle) {
  GCScope gcScope{runtime};
  ScopedNativeDepthTracker depthTracker(runtime);
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }
  // Make sure to retrieve the target and handler before any JS can execute.
  auto &slots = detail::slots(*selfHandle);
  Handle<JSObject> target = runtime.makeHandle(slots.target);
  Handle<JSObject> handler = runtime.makeHandle(slots.handler);
  CallResult<Handle<Callable>> trapRes =
      detail::findTrap(selfHandle, runtime, Predefined::deleteProperty);
  if (trapRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 7. If trap is undefined, then
  if (!*trapRes) {
    //   a. Return ? target.[[Delete]](P, Receiver).
    return JSObject::deleteComputed(target, runtime, nameValHandle);
  }
  return deleteWithTrap(runtime, nameValHandle, *trapRes, handler, target);
}

namespace {

CallResult<PseudoHandle<JSArray>> filterKeys(
    Handle<JSObject> selfHandle,
    Handle<JSArray> keys,
    Runtime &runtime,
    OwnKeysFlags okFlags) {
  assert(
      (okFlags.getIncludeNonSymbols() || okFlags.getIncludeSymbols()) &&
      "Can't exclude symbols and strings");
  // If nothing is excluded, just return the array as-is.
  if (okFlags.getIncludeNonSymbols() && okFlags.getIncludeSymbols() &&
      okFlags.getIncludeNonEnumerable()) {
    return createPseudoHandle(*keys);
  }
  // Count number of matching elements by type.
  assert(
      ((okFlags.getIncludeSymbols() ? 0 : 1) +
       (okFlags.getIncludeNonSymbols() ? 0 : 1)) == 1 &&
      "Exactly one of Symbols or non-Symbols is included here");
  bool onlySymbols = okFlags.getIncludeSymbols();
  uint32_t len = JSArray::getLength(*keys, runtime);
  uint32_t count = 0;
  // Verify this loop is alloc-free
  {
    NoAllocScope noAlloc(runtime);
    for (uint32_t i = 0; i < len; ++i) {
      if (keys->at(runtime, i).isSymbol() == onlySymbols) {
        ++count;
      }
    }
  }
  // If everything in the array matches the filter by type, return
  // the list as-is.
  if (len == count && okFlags.getIncludeNonEnumerable()) {
    return createPseudoHandle(*keys);
  }
  // Filter the desired elements we want into the result
  auto resultRes = JSArray::create(runtime, count, count);
  if (LLVM_UNLIKELY(resultRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<JSArray> resultHandle = runtime.makeHandle(std::move(*resultRes));
  MutableHandle<> elemHandle{runtime};
  uint32_t resultIndex = 0;
  GCScopeMarkerRAII marker{runtime};
  for (uint32_t i = 0; i < len; ++i) {
    marker.flush();
    SmallHermesValue elem = keys->at(runtime, i);
    if (elem.isSymbol() ? !okFlags.getIncludeSymbols()
                        : !okFlags.getIncludeNonSymbols()) {
      continue;
    }
    elemHandle = elem.unboxToHV(runtime);
    if (!okFlags.getIncludeNonEnumerable()) {
      ComputedPropertyDescriptor desc;
      CallResult<bool> propRes = JSProxy::getOwnProperty(
          selfHandle, runtime, elemHandle, desc, nullptr);
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (!*propRes || !desc.flags.enumerable) {
        continue;
      }
    }
    if (LLVM_UNLIKELY(
            JSArray::setElementAt(
                resultHandle, runtime, resultIndex++, elemHandle) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }
  assert(
      (!okFlags.getIncludeNonEnumerable() || resultIndex == count) &&
      "Expected count was not correct");
  CallResult<bool> setLenRes =
      JSArray::setLengthProperty(resultHandle, runtime, resultIndex);
  if (LLVM_UNLIKELY(setLenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return createPseudoHandle(*resultHandle);
}

} // namespace

CallResult<PseudoHandle<JSArray>> JSProxy::ownPropertyKeys(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    OwnKeysFlags okFlags) {
  struct : Locals {
    PinnedValue<JSMap> map;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScope gcScope{runtime};
  ScopedNativeDepthTracker depthTracker(runtime);
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }
  // Make sure to retrieve the target and handler before any JS can execute.
  auto &slots = detail::slots(*selfHandle);
  Handle<JSObject> target = runtime.makeHandle(slots.target);
  Handle<JSObject> handler = runtime.makeHandle(slots.handler);
  CallResult<Handle<Callable>> trapRes =
      detail::findTrap(selfHandle, runtime, Predefined::ownKeys);
  if (trapRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 6. If trap is undefined, then
  if (!*trapRes) {
    //   a. Return ? target.[[OwnPropertyKeys]]().
    CallResult<Handle<JSArray>> targetRes =
        // Include everything here, so that filterKeys has a chance to
        // make observable trap calls.
        JSObject::getOwnPropertyKeys(
            target,
            runtime,
            OwnKeysFlags()
                .plusIncludeSymbols()
                .plusIncludeNonSymbols()
                .plusIncludeNonEnumerable());
    if (targetRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    return filterKeys(selfHandle, *targetRes, runtime, okFlags);
  }
  // 7. Let trapResultArray be ? Call(trap, handler, « target »).
  CallResult<PseudoHandle<>> trapResultArrayRes = Callable::executeCall1(
      *trapRes, runtime, handler, target.getHermesValue());
  if (trapResultArrayRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  if (!vmisa<JSObject>(trapResultArrayRes->get())) {
    return runtime.raiseTypeErrorForValue(
        runtime.makeHandle(std::move(*trapResultArrayRes)),
        " ownKeys trap result is not an Object");
  }
  auto trapResultArray =
      runtime.makeHandle<JSObject>(trapResultArrayRes->get());
  // 8. Let trapResult be ? CreateListFromArrayLike(trapResultArray, « String,
  // Symbol »)
  // 9. If trapResult contains any duplicate entries, throw a TypeError
  // exception.
  CallResult<uint64_t> countRes =
      getArrayLikeLength_RJS(trapResultArray, runtime);
  if (LLVM_UNLIKELY(countRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (*countRes > UINT32_MAX) {
    return runtime.raiseRangeError(
        "Too many elements returned from ownKeys trap");
  }
  uint32_t count = static_cast<uint32_t>(*countRes);
  auto trapResultRes = JSArray::create(runtime, count, count);
  if (LLVM_UNLIKELY(trapResultRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<JSArray> trapResult = runtime.makeHandle(std::move(*trapResultRes));
  lv.map = JSMap::create(runtime, runtime.mapPrototype);
  if (LLVM_UNLIKELY(
          JSMap::initializeStorage(lv.map, runtime) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_UNLIKELY(
          createListFromArrayLike_RJS(
              trapResultArray,
              runtime,
              count,
              [&lv, &trapResult](
                  Runtime &runtime, uint64_t index, PseudoHandle<> value) {
                Handle<> valHandle = runtime.makeHandle(std::move(value));
                if (!valHandle->isString() && !valHandle->isSymbol()) {
                  return runtime.raiseTypeErrorForValue(
                      valHandle,
                      " ownKeys trap result element is not String or Symbol");
                }
                if (lv.map->has(runtime, *valHandle)) {
                  return runtime.raiseTypeErrorForValue(
                      "ownKeys trap result has duplicate ", valHandle, "");
                }
                if (LLVM_UNLIKELY(
                        JSMap::insert(lv.map, runtime, valHandle, valHandle) ==
                        ExecutionStatus::EXCEPTION))
                  return ExecutionStatus::EXCEPTION;
                return JSArray::setElementAt(
                    trapResult, runtime, index, valHandle);
              }) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 10. Let extensibleTarget be ? IsExtensible(target).
  CallResult<bool> extensibleRes = JSObject::isExtensible(target, runtime);
  if (extensibleRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 11. Let targetKeys be ? target.[[OwnPropertyKeys]]().
  CallResult<Handle<JSArray>> targetKeysRes = JSObject::getOwnPropertyKeys(
      target,
      runtime,
      OwnKeysFlags()
          .plusIncludeSymbols()
          .plusIncludeNonSymbols()
          .plusIncludeNonEnumerable());
  if (targetKeysRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<JSArray> targetKeys = *targetKeysRes;
  // 12. Assert: targetKeys is a List containing only String and Symbol values.
  // 13. Assert: targetKeys contains no duplicate entries.
  // 14. Let targetConfigurableKeys be a new empty List.
  // 15. Let targetNonconfigurableKeys be a new empty List.
  llvh::SmallSetVector<uint32_t, 8> nonConfigurable;
  // 16. For each element key of targetKeys, do
  GCScopeMarkerRAII marker{runtime};
  for (uint32_t i = 0, len = JSArray::getLength(*targetKeys, runtime); i < len;
       ++i) {
    marker.flush();
    //   a. Let desc be ? target.[[GetOwnProperty]](key).
    ComputedPropertyDescriptor desc;
    CallResult<bool> descRes = JSObject::getOwnComputedDescriptor(
        target,
        runtime,
        runtime.makeHandle(targetKeys->at(runtime, i).unboxToHV(runtime)),
        desc);
    if (descRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    //   b. If desc is not undefined and desc.[[Configurable]] is false, then
    //     i. Append key as an element of targetNonconfigurableKeys.
    //   c. Else,
    //     i. Append key as an element of targetConfigurableKeys.
    if (*descRes && !desc.flags.configurable) {
      nonConfigurable.insert(i);
    }
  }
  // 17. If extensibleTarget is true and targetNonconfigurableKeys is empty,
  // then
  if (*extensibleRes && nonConfigurable.empty()) {
    //   a. Return trapResult.
    return filterKeys(selfHandle, trapResult, runtime, okFlags);
  }
  // Hermes represents integer-index own-property keys internally as numbers,
  // but [[OwnPropertyKeys]] is specified to return only Strings and Symbols
  // (see the assert on step 12 above), and the trap result was already
  // validated to contain only Strings and Symbols. Convert any numeric index
  // keys in targetKeys to their canonical string form so the SameValue
  // comparisons below behave correctly; otherwise an integer-index key such as
  // "12345" on a non-extensible target would never match its string form in
  // the trap result. See https://github.com/facebook/hermes/issues/1609.
  {
    GCScopeMarkerRAII normalizeMarker{runtime};
    for (uint32_t i = 0, len = JSArray::getLength(*targetKeys, runtime);
         i < len;
         ++i) {
      HermesValue key = targetKeys->at(runtime, i).unboxToHV(runtime);
      if (!key.isNumber()) {
        continue;
      }
      normalizeMarker.flush();
      CallResult<PseudoHandle<StringPrimitive>> keyStrRes =
          numberToStringPrimitive(runtime, key.getNumber());
      if (LLVM_UNLIKELY(keyStrRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(
                  targetKeys,
                  runtime,
                  i,
                  runtime.makeHandle(std::move(*keyStrRes))) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    }
  }
  // 18. Let uncheckedResultKeys be a new List which is a copy of trapResult.
  // 19. For each key that is an element of targetNonconfigurableKeys, do
  //   a. If key is not an element of uncheckedResultKeys, throw a TypeError
  //   exception. b. Remove key from uncheckedResultKeys.
  auto inTrapResult = [&runtime, &trapResult](HermesValue value) {
    for (uint32_t j = 0, len = JSArray::getLength(*trapResult, runtime);
         j < len;
         ++j) {
      if (isSameValue(value, trapResult->at(runtime, j).unboxToHV(runtime))) {
        return true;
      }
    }
    return false;
  };
  for (auto i : nonConfigurable) {
    if (!inTrapResult(targetKeys->at(runtime, i).unboxToHV(runtime))) {
      return runtime.raiseTypeError(
          "ownKeys target key is non-configurable but not present in trap result");
    }
  }
  // 20. If extensibleTarget is true, return trapResult.
  if (*extensibleRes) {
    return filterKeys(selfHandle, trapResult, runtime, okFlags);
  }
  // 21. For each key that is an element of targetConfigurableKeys, do
  //   a. If key is not an element of uncheckedResultKeys, throw a TypeError
  //   exception. b. Remove key from uncheckedResultKeys.
  for (uint32_t i = 0, len = JSArray::getLength(*targetKeys, runtime); i < len;
       ++i) {
    if (nonConfigurable.count(i) > 0) {
      continue;
    }
    if (!inTrapResult(targetKeys->at(runtime, i).unboxToHV(runtime))) {
      return runtime.raiseTypeError(
          "ownKeys target is non-extensible but key is missing from trap result");
    }
  }
  // 22. If uncheckedResultKeys is not empty, throw a TypeError exception.
  if (JSArray::getLength(*targetKeys, runtime) !=
      JSArray::getLength(*trapResult, runtime)) {
    return runtime.raiseTypeError(
        "ownKeys target is non-extensible but trap result keys differ from target keys");
  }
  // 23. Return trapResult.
  return filterKeys(selfHandle, trapResult, runtime, okFlags);
}

} // namespace vm
} // namespace hermes

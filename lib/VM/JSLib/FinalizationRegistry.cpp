/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES16 26.2 FinalizationRegistry Objects
//===----------------------------------------------------------------------===//

#include "JSLibInternal.h"
#include "hermes/VM/JSFinalizationRegistry.h"
#include "hermes/VM/JSNativeFunctions.h"
#include "hermes/VM/Runtime.h"

namespace hermes {
namespace vm {

//===----------------------------------------------------------------------===//

HermesValue createFinalizationRegistryConstructor(Runtime &runtime) {
  auto prototype =
      Handle<JSObject>::vmcast(&runtime.finalizationRegistryPrototype);

  struct : Locals {
    PinnedValue<NativeConstructor> cons;
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  defineSystemConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::FinalizationRegistry),
      finalizationRegistryConstructor,
      prototype,
      1,
      lv.cons);

  DefinePropertyFlags dpf = DefinePropertyFlags::getNewNonEnumerableFlags();
  dpf.writable = 0;

  // 26.2.3.4 FinalizationRegistry.prototype [ @@toStringTag ]
  // The initial value is the String value "FinalizationRegistry".
  // This property has the attributes:
  // { [[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: true }.
  lv.str = runtime.getPredefinedString(Predefined::FinalizationRegistry);
  defineProperty(
      runtime,
      prototype,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      lv.str,
      dpf);

  // 26.2.3.2 FinalizationRegistry.prototype.register ( target, heldValue [,
  // unregisterToken ] )
  defineMethod(
      runtime,
      prototype,
      Predefined::getSymbolID(Predefined::registerStr),
      nullptr,
      finalizationRegistryPrototypeRegister,
      2);

  // 26.2.3.3 FinalizationRegistry.prototype.unregister ( unregisterToken )
  defineMethod(
      runtime,
      prototype,
      Predefined::getSymbolID(Predefined::unregister),
      nullptr,
      finalizationRegistryPrototypeUnregister,
      1);

  return lv.cons.getHermesValue();
}

// ES16 26.2.1.1 FinalizationRegistry ( cleanupCallback )
CallResult<HermesValue> finalizationRegistryConstructor(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  // 1. If NewTarget is undefined, throw a TypeError exception.
  if (!args.isConstructorCall()) {
    return runtime.raiseTypeError(
        "Constructor FinalizationRegistry requires 'new'");
  }

  auto cleanupCallback = args.getArgHandle(0);
  // 2. If IsCallable(cleanupCallback) is false, throw a TypeError exception.
  if (LLVM_UNLIKELY(!vmisa<Callable>(*cleanupCallback))) {
    return runtime.raiseTypeError(
        "FinalizationRegistry cleanup callback must be callable");
  }

  // Create the `this` for JSFinalizationRegistry.
  struct : Locals {
    PinnedValue<JSObject> selfParent;
    PinnedValue<JSFinalizationRegistry> self;
    PinnedValue<Callable> callback;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.callback = vmcast<Callable>(*cleanupCallback);

  // 3. Let finalizationRegistry be ? OrdinaryCreateFromConstructor(NewTarget,
  //    "%FinalizationRegistry.prototype%", « [[Realm]], [[CleanupCallback]],
  //    [[Cells]] »).
  if (LLVM_LIKELY(
          args.getNewTarget().getRaw() ==
          runtime.finalizationRegistryConstructor.getHermesValue().getRaw())) {
    lv.selfParent = runtime.finalizationRegistryPrototype;
  } else {
    CallResult<PseudoHandle<JSObject>> thisParentRes =
        NativeConstructor::parentForNewThis_RJS(
            runtime,
            Handle<Callable>::vmcast(&args.getNewTarget()),
            runtime.finalizationRegistryPrototype);
    if (LLVM_UNLIKELY(thisParentRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.selfParent = std::move(*thisParentRes);
  }

  // 6. Set finalizationRegistry.[[CleanupCallback]] to cleanupCallback.
  // 7. Set finalizationRegistry.[[Cells]] to a new empty List.
  lv.self = JSFinalizationRegistry::create(runtime, lv.selfParent, lv.callback);

  // 8. Return finalizationRegistry.
  return lv.self.getHermesValue();
}

// ES16 26.2.3.2 FinalizationRegistry.prototype.register ( target, heldValue
// [, unregisterToken ] )
CallResult<HermesValue> finalizationRegistryPrototypeRegister(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  // 1. Let finalizationRegistry be the *this* value.
  // 2. Perform ? RequireInternalSlot(finalizationRegistry, [[Cells]]).
  auto selfHandle = args.dyncastThis<JSFinalizationRegistry>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "FinalizationRegistry.prototype.register called on non-FinalizationRegistry object");
  }

  auto target = args.getArgHandle(0);
  auto heldValue = args.getArgHandle(1);
  // If unregisterToken is not provided, default to Undefined.
  auto unregisterToken = args.getArgHandle(2);

  // 3. If CanBeHeldWeakly(target) is false, throw a TypeError exception.
  if (LLVM_UNLIKELY(!canBeHeldWeakly(runtime, *target))) {
    return runtime.raiseTypeError(
        "target must be an object or non-registered symbol");
  }

  // 4. If SameValue(target, heldValue) is true, throw a TypeError exception.
  // Since target is either Object or Symbol, we can simply compare raw bits.
  if (LLVM_UNLIKELY(target->getRaw() == heldValue->getRaw())) {
    return runtime.raiseTypeError("target and heldValue cannot be the same");
  }

  // 5. If CanBeHeldWeakly(unregisterToken) is false, then
  //    a. If unregisterToken is not undefined, throw a TypeError exception.
  if (!unregisterToken->isUndefined()) {
    if (LLVM_UNLIKELY(!canBeHeldWeakly(runtime, *unregisterToken))) {
      return runtime.raiseTypeError(
          "unregisterToken must be an object, non-registered symbol, or undefined");
    }
  }

  // 6. Let cell be the Record { [[WeakRefTarget]]: target, [[HeldValue]]:
  //    heldValue, [[UnregisterToken]]: unregisterToken }.
  // 7. Append cell to finalizationRegistry.[[Cells]].
  if (LLVM_UNLIKELY(
          JSFinalizationRegistry::registerCell(
              selfHandle, runtime, target, heldValue, unregisterToken) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;

  // 8. Return undefined.
  return HermesValue::encodeUndefinedValue();
}

// ES16 26.2.3.3 FinalizationRegistry.prototype.unregister ( unregisterToken )
CallResult<HermesValue> finalizationRegistryPrototypeUnregister(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  // 1. Let finalizationRegistry be the this value.
  // 2. Perform ? RequireInternalSlot(finalizationRegistry, [[Cells]]).
  auto selfHandle = args.dyncastThis<JSFinalizationRegistry>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "unregister called on non-FinalizationRegistry object");
  }

  auto unregisterToken = args.getArgHandle(0);

  // 3. If CanBeHeldWeakly(unregisterToken) is false, throw a TypeError
  // exception.
  if (LLVM_UNLIKELY(!canBeHeldWeakly(runtime, *unregisterToken))) {
    return runtime.raiseTypeError(
        "unregisterToken must be an object or non-registered symbol");
  }

  // Remove cells with matched unregister token.
  bool removed = selfHandle->unregisterCells(runtime, unregisterToken);

  // 6. Return removed.
  return HermesValue::encodeBoolValue(removed);
}

} // namespace vm
} // namespace hermes

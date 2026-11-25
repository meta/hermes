/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSLibInternal.h"

#include "hermes/VM/CellKind.h"
#include "hermes/VM/JSWeakMapImpl.h"
#include "hermes/VM/StringPrimitive.h"

namespace hermes {
namespace vm {

HermesValue createWeakMapConstructor(Runtime &runtime) {
  auto weakMapPrototype = Handle<JSObject>::vmcast(&runtime.weakMapPrototype);

  struct : public Locals {
    PinnedValue<NativeConstructor> cons;
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  defineMethod(
      runtime,
      weakMapPrototype,
      Predefined::getSymbolID(Predefined::deleteStr),
      nullptr,
      weakMapPrototypeDelete,
      1);

  defineMethod(
      runtime,
      weakMapPrototype,
      Predefined::getSymbolID(Predefined::get),
      nullptr,
      weakMapPrototypeGet,
      1);

  defineMethod(
      runtime,
      weakMapPrototype,
      Predefined::getSymbolID(Predefined::getOrInsert),
      nullptr,
      weakMapPrototypeGetOrInsert,
      2);

  defineMethod(
      runtime,
      weakMapPrototype,
      Predefined::getSymbolID(Predefined::getOrInsertComputed),
      nullptr,
      weakMapPrototypeGetOrInsertComputed,
      2);

  defineMethod(
      runtime,
      weakMapPrototype,
      Predefined::getSymbolID(Predefined::has),
      nullptr,
      weakMapPrototypeHas,
      1);

  defineMethod(
      runtime,
      weakMapPrototype,
      Predefined::getSymbolID(Predefined::set),
      nullptr,
      weakMapPrototypeSet,
      2);

  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  lv.str = runtime.getPredefinedString(Predefined::WeakMap);
  defineProperty(
      runtime,
      weakMapPrototype,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      lv.str,
      dpf);

  defineSystemConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::WeakMap),
      weakMapConstructor,
      weakMapPrototype,
      0,
      lv.cons);

  // ES6.0 23.3.3.1
  defineProperty(
      runtime,
      weakMapPrototype,
      Predefined::getSymbolID(Predefined::constructor),
      lv.cons);

  return lv.cons.getHermesValue();
}

CallResult<HermesValue> weakMapConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};

  if (LLVM_UNLIKELY(!args.isConstructorCall())) {
    return runtime.raiseTypeError("WeakMap must be called as a constructor");
  }

  struct : public Locals {
    PinnedValue<JSObject> selfParent;
    PinnedValue<JSWeakMap> self;
    PinnedValue<JSObject> next;
    PinnedValue<JSObject> nextItem;
    PinnedValue<> keyHandle;
    PinnedValue<> valueHandle;
    PinnedValue<Callable> adder;
    CheckedIteratorRecordStorage iterStorage;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  if (LLVM_LIKELY(
          args.getNewTarget().getRaw() ==
          runtime.weakMapConstructor.getHermesValue().getRaw())) {
    lv.selfParent = runtime.weakMapPrototype;
  } else {
    CallResult<PseudoHandle<JSObject>> thisParentRes =
        NativeConstructor::parentForNewThis_RJS(
            runtime,
            Handle<Callable>::vmcast(&args.getNewTarget()),
            runtime.weakMapPrototype);
    if (LLVM_UNLIKELY(thisParentRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.selfParent = std::move(*thisParentRes);
  }
  auto selfRes = JSWeakMap::create(runtime, lv.selfParent);
  if (LLVM_UNLIKELY(selfRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.self = std::move(*selfRes);

  if (args.getArgCount() == 0 || args.getArg(0).isUndefined() ||
      args.getArg(0).isNull()) {
    return lv.self.getHermesValue();
  }

  auto propRes = JSObject::getNamed_RJS(
      lv.self, runtime, Predefined::getSymbolID(Predefined::set));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (auto callable = dyn_vmcast<Callable>(propRes->getHermesValue())) {
    lv.adder = callable;
  } else {
    return runtime.raiseTypeError("Property 'set' for WeakMap is not callable");
  }

  auto iterRes =
      getCheckedIterator(runtime, args.getArgHandle(0), lv.iterStorage);
  if (LLVM_UNLIKELY(iterRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto iteratorRecord = *iterRes;

  auto marker = gcScope.createMarker();

  for (;;) {
    gcScope.flushToMarker(marker);
    auto nextRes = iteratorStep(runtime, iteratorRecord);
    if (LLVM_UNLIKELY(nextRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (!*nextRes) {
      return lv.self.getHermesValue();
    }
    lv.next = std::move(*nextRes);
    auto nextItemRes = JSObject::getNamed_RJS(
        lv.next, runtime, Predefined::getSymbolID(Predefined::value));
    if (LLVM_UNLIKELY(nextItemRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (!vmisa<JSObject>(nextItemRes->get())) {
      (void)runtime.raiseTypeError(
          "WeakMap([iterable]) elements must be objects");
      return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
    }
    lv.nextItem = vmcast<JSObject>(nextItemRes->get());
    auto keyRes = getIndexed_RJS(runtime, lv.nextItem, 0);
    if (LLVM_UNLIKELY(keyRes == ExecutionStatus::EXCEPTION)) {
      return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
    }
    lv.keyHandle = std::move(*keyRes);
    auto valueRes = getIndexed_RJS(runtime, lv.nextItem, 1);
    if (LLVM_UNLIKELY(valueRes == ExecutionStatus::EXCEPTION)) {
      return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
    }
    lv.valueHandle = std::move(*valueRes);
    if (LLVM_UNLIKELY(
            Callable::executeCall2(
                lv.adder,
                runtime,
                lv.self,
                lv.keyHandle.getHermesValue(),
                lv.valueHandle.getHermesValue()) ==
            ExecutionStatus::EXCEPTION)) {
      return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
    }
  }
}

CallResult<HermesValue> weakMapPrototypeDelete(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto M = args.dyncastThis<JSWeakMap>();
  if (LLVM_UNLIKELY(!M)) {
    return runtime.raiseTypeError(
        "WeakMap.prototype.delete can only be called on a WeakMap");
  }

  auto key = args.getArgHandle(0);
  if (LLVM_UNLIKELY(!canBeHeldWeakly(runtime, *key))) {
    return HermesValue::encodeBoolValue(false);
  }

  return HermesValue::encodeBoolValue(JSWeakMap::deleteValue(M, runtime, key));
}

CallResult<HermesValue> weakMapPrototypeGet(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto M = args.dyncastThis<JSWeakMap>();
  if (LLVM_UNLIKELY(!M)) {
    return runtime.raiseTypeError(
        "WeakMap.prototype.get can only be called on a WeakMap");
  }

  auto key = args.getArgHandle(0);
  if (LLVM_UNLIKELY(!canBeHeldWeakly(runtime, *key))) {
    return HermesValue::encodeUndefinedValue();
  }

  return JSWeakMap::getValue(M, runtime, key);
}

/// 24.3.3.4 WeakMap.prototype.getOrInsert(key, value)
CallResult<HermesValue> weakMapPrototypeGetOrInsert(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. Let M be the this value.
  auto M = args.dyncastThis<JSWeakMap>();
  // 2. Perform ? RequireInternalSlot(M, [[WeakMapData]]).
  if (LLVM_UNLIKELY(!M)) {
    return runtime.raiseTypeError(
        "WeakMap.prototype.getOrInsert can only be called on a WeakMap");
  }

  auto key = args.getArgHandle(0);
  // 3. If CanBeHeldWeakly(key) is false, throw a TypeError exception.
  if (LLVM_UNLIKELY(!canBeHeldWeakly(runtime, *key))) {
    return runtime.raiseTypeError(
        "WeakMap key must be an Object or non-registered Symbol");
  }

  // rest of the steps in the following call.
  return JSWeakMap::getOrInsert(M, runtime, key, args.getArgHandle(1));
}

/// 24.3.3.5 WeakMap.prototype.getOrInsertComputed(key, callback)
CallResult<HermesValue> weakMapPrototypeGetOrInsertComputed(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. Let M be the this value.
  auto M = args.dyncastThis<JSWeakMap>();
  // 2. Perform ? RequireInternalSlot(M, [[WeakMapData]]).
  if (LLVM_UNLIKELY(!M)) {
    return runtime.raiseTypeError(
        "WeakMap.prototype.getOrInsertComputed can only be called on a WeakMap");
  }

  auto key = args.getArgHandle(0);
  // 3. If CanBeHeldWeakly(key) is false, throw a TypeError exception.
  if (LLVM_UNLIKELY(!canBeHeldWeakly(runtime, *key))) {
    return runtime.raiseTypeError(
        "WeakMap key must be an Object or non-registered Symbol");
  }

  auto callback = args.dyncastArg<Callable>(1);
  // 4. If IsCallable(callback) is false, throw a TypeError exception.
  if (LLVM_UNLIKELY(!callback)) {
    return runtime.raiseTypeError(
        "callback must be Callable in WeakMap.prototype.getOrInsertComputed");
  }

  // rest of the steps in the following call.
  return JSWeakMap::getOrInsertComputed(M, runtime, key, callback);
}

CallResult<HermesValue> weakMapPrototypeHas(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto M = args.dyncastThis<JSWeakMap>();
  if (LLVM_UNLIKELY(!M)) {
    return runtime.raiseTypeError(
        "WeakMap.prototype.has can only be called on a WeakMap");
  }

  auto key = args.getArgHandle(0);
  if (LLVM_UNLIKELY(!canBeHeldWeakly(runtime, *key))) {
    return HermesValue::encodeBoolValue(false);
  }

  return HermesValue::encodeBoolValue(JSWeakMap::hasValue(M, runtime, key));
}

CallResult<HermesValue> weakMapPrototypeSet(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto M = args.dyncastThis<JSWeakMap>();
  if (LLVM_UNLIKELY(!M)) {
    return runtime.raiseTypeError(
        "WeakMap.prototype.set can only be called on a WeakMap");
  }

  auto key = args.getArgHandle(0);
  if (LLVM_UNLIKELY(!canBeHeldWeakly(runtime, *key))) {
    return runtime.raiseTypeError(
        "WeakMap key must be an Object or non-registered Symbol");
  }

  JSWeakMap::setValue(M, runtime, key, args.getArgHandle(1));
  return M.getHermesValue();
}

} // namespace vm
} // namespace hermes

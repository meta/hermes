/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES6.0 23.1 Initialize the Map constructor.
//===----------------------------------------------------------------------===//

#include "JSLibInternal.h"

#include "hermes/VM/StringPrimitive.h"

namespace hermes {
namespace vm {

Handle<JSObject> createMapConstructor(Runtime &runtime) {
  auto mapPrototype = Handle<JSObject>::vmcast(&runtime.mapPrototype);

  // Map.prototype.xxx methods.
  defineMethod(
      runtime,
      mapPrototype,
      Predefined::getSymbolID(Predefined::clear),
      nullptr,
      mapPrototypeClear,
      0);

  defineMethod(
      runtime,
      mapPrototype,
      Predefined::getSymbolID(Predefined::deleteStr),
      nullptr,
      mapPrototypeDelete,
      1);

  defineMethod(
      runtime,
      mapPrototype,
      Predefined::getSymbolID(Predefined::entries),
      nullptr,
      mapPrototypeEntries,
      0);

  defineMethod(
      runtime,
      mapPrototype,
      Predefined::getSymbolID(Predefined::forEach),
      nullptr,
      mapPrototypeForEach,
      1);

  defineMethod(
      runtime,
      mapPrototype,
      Predefined::getSymbolID(Predefined::get),
      nullptr,
      mapPrototypeGet,
      1);

  defineMethod(
      runtime,
      mapPrototype,
      Predefined::getSymbolID(Predefined::has),
      nullptr,
      mapPrototypeHas,
      1);

  defineMethod(
      runtime,
      mapPrototype,
      Predefined::getSymbolID(Predefined::keys),
      nullptr,
      mapPrototypeKeys,
      0);

  defineMethod(
      runtime,
      mapPrototype,
      Predefined::getSymbolID(Predefined::set),
      nullptr,
      mapPrototypeSet,
      2);

  defineAccessor(
      runtime,
      mapPrototype,
      Predefined::getSymbolID(Predefined::size),
      nullptr,
      mapPrototypeSizeGetter,
      nullptr,
      false,
      true);

  defineMethod(
      runtime,
      mapPrototype,
      Predefined::getSymbolID(Predefined::values),
      nullptr,
      mapPrototypeValues,
      0);

  DefinePropertyFlags dpf = DefinePropertyFlags::getNewNonEnumerableFlags();

  PseudoHandle<> propValue = runtime.ignoreAllocationFailure(
      JSObject::getNamed_RJS(
          mapPrototype, runtime, Predefined::getSymbolID(Predefined::entries)));
  runtime.ignoreAllocationFailure(
      JSObject::defineOwnProperty(
          mapPrototype,
          runtime,
          Predefined::getSymbolID(Predefined::SymbolIterator),
          dpf,
          runtime.makeHandle<NativeFunction>(propValue.get())));

  dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  defineProperty(
      runtime,
      mapPrototype,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      runtime.getPredefinedStringHandle(Predefined::Map),
      dpf);

  auto cons = defineSystemConstructor<JSMap>(
      runtime,
      Predefined::getSymbolID(Predefined::Map),
      mapConstructor,
      mapPrototype,
      0,
      CellKind::JSMapKind);

  // Map.xxx static functions
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::groupBy),
      nullptr,
      mapGroupBy,
      2);

  return cons;
}

CallResult<HermesValue>
mapConstructor(void *, Runtime &runtime, NativeArgs args) {
  GCScope gcScope{runtime};
  if (LLVM_UNLIKELY(!args.isConstructorCall())) {
    return runtime.raiseTypeError("Constructor Map requires 'new'");
  }
  auto selfHandle = args.dyncastThis<JSMap>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError("Map Constructor only applies to Map object");
  }
  JSMap::initializeStorage(selfHandle, runtime);
  if (args.getArgCount() == 0 || args.getArg(0).isUndefined() ||
      args.getArg(0).isNull()) {
    return selfHandle.getHermesValue();
  }

  auto propRes = JSObject::getNamed_RJS(
      selfHandle, runtime, Predefined::getSymbolID(Predefined::set));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // ES6.0 23.1.1.1.7: Cache adder across all iterations of the loop.
  auto adder =
      Handle<Callable>::dyn_vmcast(runtime.makeHandle(std::move(*propRes)));
  if (!adder) {
    return runtime.raiseTypeError("Property 'set' for Map is not callable");
  }

  return addEntriesFromIterable(
      runtime,
      selfHandle,
      args.getArgHandle(0),
      [&runtime, selfHandle, adder](Runtime &, Handle<> key, Handle<> value) {
        return Callable::executeCall2(
                   adder,
                   runtime,
                   selfHandle,
                   key.getHermesValue(),
                   value.getHermesValue())
            .getStatus();
      });
}

CallResult<HermesValue>
mapPrototypeClear(void *, Runtime &runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSMap>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Map object called on Map.prototype.clear");
  }
  JSMap::clear(selfHandle, runtime);
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue>
mapPrototypeDelete(void *, Runtime &runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSMap>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Map object called on Map.prototype.delete");
  }
  return HermesValue::encodeBoolValue(
      JSMap::deleteKey(selfHandle, runtime, args.getArgHandle(0)));
}

CallResult<HermesValue>
mapPrototypeEntries(void *, Runtime &runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSMap>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Map object called on Map.prototype.entries");
  }
  auto iterator = runtime.makeHandle(
      JSMapIterator::create(
          runtime, Handle<JSObject>::vmcast(&runtime.mapIteratorPrototype)));
  iterator->initializeIterator(runtime, selfHandle, IterationKind::Entry);
  return iterator.getHermesValue();
}

CallResult<HermesValue>
mapPrototypeForEach(void *, Runtime &runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSMap>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Map object called on Map.prototype.forEach");
  }
  auto callbackfn = args.dyncastArg<Callable>(0);
  if (LLVM_UNLIKELY(!callbackfn)) {
    return runtime.raiseTypeError(
        "callbackfn must be Callable in Map.prototype.forEach");
  }
  auto thisArg = args.getArgHandle(1);
  if (JSMap::forEach(selfHandle, runtime, callbackfn, thisArg) ==
      ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue>
mapPrototypeGet(void *, Runtime &runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSMap>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError("Non-Map object called on Map.prototype.get");
  }
  return JSMap::getValue(selfHandle, runtime, args.getArgHandle(0));
}

CallResult<HermesValue>
mapPrototypeHas(void *, Runtime &runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSMap>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError("Non-Map object called on Map.prototype.has");
  }
  return HermesValue::encodeBoolValue(
      JSMap::hasKey(selfHandle, runtime, args.getArgHandle(0)));
}

CallResult<HermesValue>
mapPrototypeKeys(void *, Runtime &runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSMap>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Map object called on Map.prototype.keys");
  }

  auto iterator = runtime.makeHandle(
      JSMapIterator::create(
          runtime, Handle<JSObject>::vmcast(&runtime.mapIteratorPrototype)));
  iterator->initializeIterator(runtime, selfHandle, IterationKind::Key);
  return iterator.getHermesValue();
}

// ES12 23.1.3.9 Map.prototype.set ( key, value )
CallResult<HermesValue>
mapPrototypeSet(void *, Runtime &runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSMap>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError("Non-Map object called on Map.prototype.set");
  }
  auto keyHandle = args.getArgHandle(0);
  // 5. If key is -0, set key to +0.
  // N.B. in the case of Map, only key should be normalized but not the value.
  auto key = keyHandle->isNumber() && keyHandle->getNumber() == 0
      ? HandleRootOwner::getZeroValue()
      : keyHandle;
  JSMap::addValue(selfHandle, runtime, key, args.getArgHandle(1));
  return selfHandle.getHermesValue();
}

CallResult<HermesValue>
mapPrototypeSizeGetter(void *, Runtime &runtime, NativeArgs args) {
  auto self = dyn_vmcast<JSMap>(args.getThisArg());
  if (LLVM_UNLIKELY(!self)) {
    return runtime.raiseTypeError(
        "Non-Map object called on Map.prototype.size");
  }
  return HermesValue::encodeUntrustedNumberValue(JSMap::getSize(self, runtime));
}

CallResult<HermesValue>
mapPrototypeValues(void *, Runtime &runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSMap>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Map object called on Map.prototype.values");
  }
  auto iterator = runtime.makeHandle(
      JSMapIterator::create(
          runtime, Handle<JSObject>::vmcast(&runtime.mapIteratorPrototype)));
  iterator->initializeIterator(runtime, selfHandle, IterationKind::Value);
  return iterator.getHermesValue();
}

Handle<JSObject> createMapIteratorPrototype(Runtime &runtime) {
  auto parentHandle = runtime.makeHandle(
      JSObject::create(
          runtime, Handle<JSObject>::vmcast(&runtime.iteratorPrototype)));
  defineMethod(
      runtime,
      parentHandle,
      Predefined::getSymbolID(Predefined::next),
      nullptr,
      mapIteratorPrototypeNext,
      0);

  auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  defineProperty(
      runtime,
      parentHandle,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      runtime.getPredefinedStringHandle(Predefined::MapIterator),
      dpf);

  return parentHandle;
}

CallResult<HermesValue>
mapIteratorPrototypeNext(void *, Runtime &runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSMapIterator>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-MapIterator object called on MapIterator.prototype.next");
  }

  auto cr = JSMapIterator::nextElement(selfHandle, runtime);
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return *cr;
}

CallResult<HermesValue>
mapGroupBy(void *, Runtime &runtime, NativeArgs args) {
  GCScope gcScope{runtime};

  Handle<> items = args.getArgHandle(0);
  Handle<Callable> grouperFunc = args.dyncastArg<Callable>(1);

  // 1. Perform ? RequireObjectCoercible(items).
  if (LLVM_UNLIKELY(items->isNull() || items->isUndefined())) {
    return runtime.raiseTypeError(
        "groupBy first argument is not coercible to Object");
  }

  // 2. If IsCallable(callbackfn) is false, throw a TypeError exception.
  if (LLVM_UNLIKELY(!grouperFunc)) {
    return runtime.raiseTypeError(
        "groupBy second argument must be callable");
  }

  // 4. Let iteratorRecord be ? GetIterator(items, sync).
  auto iteratorRecordRes = getIterator(runtime, items);
  if (LLVM_UNLIKELY(iteratorRecordRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto iteratorRecord = *iteratorRecordRes;

  // 5. Let k be 0.
  size_t k = 0;

  auto O = runtime.makeHandle(JSMap::create(runtime, Handle<JSObject>::vmcast(&runtime.mapPrototype)));
  JSMap::initializeStorage(O, runtime);
  Handle<HermesValue> callbackThis = runtime.makeHandle(HermesValue::encodeUndefinedValue());

  MutableHandle<> objectArrayHandle{runtime};
  MutableHandle<HermesValue> groupKey{runtime};
  MutableHandle<JSArray> targetGroupArray{runtime};
  MutableHandle<> targetGroupIndex{runtime};
  MutableHandle<JSObject> tmpHandle{runtime};
  MutableHandle<> valueHandle{runtime};
  auto marker = gcScope.createMarker();

  // 6. Repeat,
  // Check the length of the array after every iteration,
  // to allow for the fact that the length could be modified during iteration.
  for (;; k++) {
    CallResult<Handle<JSObject>> nextRes = iteratorStep(runtime, iteratorRecord);
    if (LLVM_UNLIKELY(nextRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    if (!*nextRes) {
      // Done with iteration.
      break;
    }

    tmpHandle = vmcast<JSObject>(nextRes->getHermesValue());
    auto nextValueRes = JSObject::getNamed_RJS(
        tmpHandle, runtime, Predefined::getSymbolID(Predefined::value));
    if (LLVM_UNLIKELY(nextValueRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    valueHandle = runtime.makeHandle(std::move(*nextValueRes));

    // compute key for current element
    auto keyRes = Callable::executeCall2(grouperFunc, runtime, callbackThis, valueHandle.getHermesValue(), HermesValue::encodeTrustedNumberValue(k));
    if (LLVM_UNLIKELY(keyRes == ExecutionStatus::EXCEPTION)) {
      return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
    }

    groupKey = runtime.makeHandle(std::move(*keyRes));

    // make it a property key
    auto propertyKeyRes = toPropertyKey(runtime, groupKey);
    if (LLVM_UNLIKELY(propertyKeyRes == ExecutionStatus::EXCEPTION)) {
      return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
    }
    auto propertyKey = *propertyKeyRes;

    // new group key, no array in object yet so create it
    if (!JSMap::hasKey(O, runtime, propertyKey)) {
      auto targetGroupArrayRes = JSArray::create(runtime, 0, 0);
      if (LLVM_UNLIKELY(targetGroupArrayRes == ExecutionStatus::EXCEPTION)) {
        return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
      }

      targetGroupArray = std::move(*targetGroupArrayRes);

      // group already created, get the array
      JSMap::addValue(O, runtime, propertyKey, targetGroupArray);
    } else {
      objectArrayHandle = runtime.makeHandle(std::move(JSMap::getValue(O, runtime, propertyKey)));
      targetGroupArray = Handle<JSArray>::dyn_vmcast(objectArrayHandle);
    }

    targetGroupIndex = HermesValue::encodeTrustedNumberValue(JSArray::getLength(*targetGroupArray, runtime));

    if (LLVM_UNLIKELY(
            JSObject::putComputed_RJS(
                targetGroupArray, runtime,
                targetGroupIndex, valueHandle, PropOpFlags().plusThrowOnError()) ==
            ExecutionStatus::EXCEPTION)) {
      return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
    }

    gcScope.flushToMarker(marker);
  }

  // 8. Return O.
  return O.getHermesValue();
}
} // namespace vm
} // namespace hermes

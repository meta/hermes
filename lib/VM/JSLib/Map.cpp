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

HermesValue createMapConstructor(Runtime &runtime) {
  auto mapPrototype = Handle<JSObject>::vmcast(&runtime.mapPrototype);

  struct : public Locals {
    PinnedValue<NativeConstructor> cons;
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);

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

  runtime.mapPrototypeEntries = defineMethod(
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
      Predefined::getSymbolID(Predefined::getOrInsert),
      nullptr,
      mapPrototypeGetOrInsert,
      2);

  defineMethod(
      runtime,
      mapPrototype,
      Predefined::getSymbolID(Predefined::getOrInsertComputed),
      nullptr,
      mapPrototypeGetOrInsertComputed,
      2);

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

  runtime.mapPrototypeSet = defineMethod(
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
  runtime.ignoreAllocationFailure(
      JSObject::defineOwnProperty(
          mapPrototype,
          runtime,
          Predefined::getSymbolID(Predefined::SymbolIterator),
          dpf,
          runtime.mapPrototypeEntries));

  dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  lv.str = runtime.getPredefinedString(Predefined::Map);
  defineProperty(
      runtime,
      mapPrototype,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      lv.str,
      dpf);

  defineSystemConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::Map),
      mapConstructor,
      mapPrototype,
      0,
      lv.cons);

  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::groupBy),
      nullptr,
      mapGroupBy,
      2);

  return lv.cons.getHermesValue();
}

/// Populate the Map with the contents of the source Map.
/// \param target the Map to populate (newly constructed).
/// \param src the Map to pull the entries from.
/// \return the newly populated map.
static ExecutionStatus
mapFromMapFastPath(Runtime &runtime, Handle<JSMap> target, Handle<JSMap> src) {
  // TODO: This can be improved further by avoiding any rehashes and the
  // SmallHermesValue unbox/boxing. We should be able to make an
  // OrderedHashMap::clone that initializes based on an existing Map
  // and clones all entries directly somehow.
  struct : public Locals {
    PinnedValue<> keyHandle;
    PinnedValue<> valueHandle;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  return JSMap::forEachNative(
      src,
      runtime,
      [&target, &lv](Runtime &runtime, HermesValue keyHV, HermesValue valueHV)
          -> ExecutionStatus {
        lv.keyHandle = keyHV;
        lv.valueHandle = valueHV;
        if (LLVM_UNLIKELY(
                JSMap::insert(target, runtime, lv.keyHandle, lv.valueHandle) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        return ExecutionStatus::RETURNED;
      });
}

CallResult<HermesValue> mapConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};
  if (LLVM_UNLIKELY(!args.isConstructorCall())) {
    return runtime.raiseTypeError("Constructor Map requires 'new'");
  }

  struct : public Locals {
    PinnedValue<JSObject> selfParent;
    PinnedValue<JSMap> self;
    PinnedValue<> adderProp;
    PinnedValue<SymbolID> iteratorSymbol;
    PinnedValue<Callable> iterMethod;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  if (LLVM_LIKELY(
          args.getNewTarget().getRaw() ==
          runtime.mapConstructor.getHermesValue().getRaw())) {
    lv.selfParent = runtime.mapPrototype;
  } else {
    CallResult<PseudoHandle<JSObject>> thisParentRes =
        NativeConstructor::parentForNewThis_RJS(
            runtime,
            Handle<Callable>::vmcast(&args.getNewTarget()),
            runtime.mapPrototype);
    if (LLVM_UNLIKELY(thisParentRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.selfParent = std::move(*thisParentRes);
  }

  lv.self = JSMap::create(runtime, lv.selfParent);

  if (LLVM_UNLIKELY(
          JSMap::initializeStorage(lv.self, runtime) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (args.getArgCount() == 0 || args.getArg(0).isUndefined() ||
      args.getArg(0).isNull()) {
    return lv.self.getHermesValue();
  }

  auto propRes = JSObject::getNamed_RJS(
      lv.self, runtime, Predefined::getSymbolID(Predefined::set));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // ES6.0 23.1.1.1.7: Cache adder across all iterations of the loop.
  lv.adderProp = std::move(*propRes);
  auto adder = Handle<Callable>::dyn_vmcast(Handle<>{lv.adderProp});
  if (!adder) {
    return runtime.raiseTypeError("Property 'set' for Map is not callable");
  }

  lv.iteratorSymbol = Predefined::getSymbolID(Predefined::SymbolIterator);
  auto iterMethodRes =
      getMethod(runtime, args.getArgHandle(0), lv.iteratorSymbol);
  if (LLVM_UNLIKELY(iterMethodRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (!vmisa<Callable>(iterMethodRes->getHermesValue())) {
    return runtime.raiseTypeError("iterator method is not callable");
  }
  lv.iterMethod.castAndSetHermesValue<Callable>(
      iterMethodRes->getHermesValue());

  // Check and run fast path.
  if (LLVM_LIKELY(
          adder.getHermesValue().getRaw() ==
          runtime.mapPrototypeSet.getHermesValue().getRaw())) {
    // If the iterable is a Map with the original iterator,
    // then we can do for-loop.
    if (Handle<JSMap> inputMap = args.dyncastArg<JSMap>(0); inputMap &&
        LLVM_LIKELY(lv.iterMethod.getHermesValue().getRaw() ==
                    runtime.mapPrototypeEntries.getHermesValue().getRaw())) {
      if (LLVM_UNLIKELY(
              mapFromMapFastPath(runtime, lv.self, inputMap) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      return lv.self.getHermesValue();
    }

    // TODO: Fast path for JSArray input.
  }

  return addEntriesFromIterable(
      runtime,
      lv.self,
      args.getArgHandle(0),
      Handle<Callable>{lv.iterMethod},
      [&runtime, &self = lv.self, adder](
          Runtime &, Handle<> key, Handle<> value) {
        return Callable::executeCall2(
                   adder,
                   runtime,
                   self,
                   key.getHermesValue(),
                   value.getHermesValue())
            .getStatus();
      });
}

CallResult<HermesValue> mapPrototypeClear(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto selfHandle = args.dyncastThis<JSMap>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Map object called on Map.prototype.clear");
  }
  if (LLVM_UNLIKELY(
          JSMap::clear(selfHandle, runtime) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue> mapPrototypeDelete(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto selfHandle = args.dyncastThis<JSMap>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Map object called on Map.prototype.delete");
  }
  return HermesValue::encodeBoolValue(
      JSMap::erase(selfHandle, runtime, args.getArgHandle(0)));
}

CallResult<HermesValue> mapPrototypeEntries(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto selfHandle = args.dyncastThis<JSMap>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Map object called on Map.prototype.entries");
  }
  struct : public Locals {
    PinnedValue<JSMapIterator> iterator;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.iterator.castAndSetHermesValue<JSMapIterator>(
      JSMapIterator::create(
          runtime, Handle<JSObject>::vmcast(&runtime.mapIteratorPrototype))
          .getHermesValue());
  lv.iterator->initializeIterator(runtime, selfHandle, IterationKind::Entry);
  return lv.iterator.getHermesValue();
}

CallResult<HermesValue> mapPrototypeForEach(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
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

CallResult<HermesValue> mapPrototypeGet(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto *self = dyn_vmcast<JSMap>(args.getThisArg());
  if (LLVM_UNLIKELY(!self)) {
    return runtime.raiseTypeError("Non-Map object called on Map.prototype.get");
  }
  return self->get(runtime, args.getArg(0)).unboxToHV(runtime);
}

/// ES2026 24.1.3.7 Map.prototype.getOrInsert(key, value)
CallResult<HermesValue> mapPrototypeGetOrInsert(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. Let M be the this value.
  auto M = args.dyncastThis<JSMap>();
  // 2. Perform ? RequireInternalSlot(M, [[MapData]]).
  if (LLVM_UNLIKELY(!M)) {
    return runtime.raiseTypeError(
        "Non-Map object called on Map.prototype.getOrInsert");
  }
  struct : public Locals {
    PinnedValue<> key;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  // 3. Set key to CanonicalizeKeyedCollectionKey(key).
  lv.key = canonicalizeKeyedCollectionKey(args.getArg(0));
  // 4-7 handled in the following call.
  return JSMap::getOrInsert(M, runtime, lv.key, args.getArgHandle(1));
}

/// ES2026 24.1.3.8 Map.prototype.getOrInsertComputed(key, callback)
CallResult<HermesValue> mapPrototypeGetOrInsertComputed(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. Let M be the this value.
  auto M = args.dyncastThis<JSMap>();
  // 2. Perform ? RequireInternalSlot(M, [[MapData]]).
  if (LLVM_UNLIKELY(!M)) {
    return runtime.raiseTypeError(
        "Non-Map object called on Map.prototype.getOrInsertComputed");
  }
  auto callback = args.dyncastArg<Callable>(1);
  // 3. If IsCallable(callback) is false, throw a TypeError exception.
  if (LLVM_UNLIKELY(!callback)) {
    return runtime.raiseTypeError(
        "callback must be Callable in Map.prototype.getOrInsertComputed");
  }
  struct : public Locals {
    PinnedValue<> key;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  // 4. Set key to CanonicalizeKeyedCollectionKey(key).
  lv.key = canonicalizeKeyedCollectionKey(args.getArg(0));
  // 5-11 handled in the following call.
  return JSMap::getOrInsertComputed(M, runtime, lv.key, callback);
}

CallResult<HermesValue> mapPrototypeHas(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto *self = dyn_vmcast<JSMap>(args.getThisArg());
  if (LLVM_UNLIKELY(!self)) {
    return runtime.raiseTypeError("Non-Map object called on Map.prototype.has");
  }
  return HermesValue::encodeBoolValue(self->has(runtime, args.getArg(0)));
}

CallResult<HermesValue> mapPrototypeKeys(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto selfHandle = args.dyncastThis<JSMap>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Map object called on Map.prototype.keys");
  }

  struct : public Locals {
    PinnedValue<JSMapIterator> iterator;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.iterator.castAndSetHermesValue<JSMapIterator>(
      JSMapIterator::create(
          runtime, Handle<JSObject>::vmcast(&runtime.mapIteratorPrototype))
          .getHermesValue());
  lv.iterator->initializeIterator(runtime, selfHandle, IterationKind::Key);
  return lv.iterator.getHermesValue();
}

// ES12 23.1.3.9 Map.prototype.set ( key, value )
CallResult<HermesValue> mapPrototypeSet(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
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
  if (LLVM_UNLIKELY(
          JSMap::insert(selfHandle, runtime, key, args.getArgHandle(1)) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return selfHandle.getHermesValue();
}

CallResult<HermesValue> mapPrototypeSizeGetter(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto self = dyn_vmcast<JSMap>(args.getThisArg());
  if (LLVM_UNLIKELY(!self)) {
    return runtime.raiseTypeError(
        "Non-Map object called on Map.prototype.size");
  }
  return HermesValue::encodeTrustedNumberValue(self->size());
}

CallResult<HermesValue> mapPrototypeValues(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto selfHandle = args.dyncastThis<JSMap>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Map object called on Map.prototype.values");
  }
  struct : public Locals {
    PinnedValue<JSMapIterator> iterator;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.iterator.castAndSetHermesValue<JSMapIterator>(
      JSMapIterator::create(
          runtime, Handle<JSObject>::vmcast(&runtime.mapIteratorPrototype))
          .getHermesValue());
  lv.iterator->initializeIterator(runtime, selfHandle, IterationKind::Value);
  return lv.iterator.getHermesValue();
}

HermesValue createMapIteratorPrototype(Runtime &runtime) {
  struct : public Locals {
    PinnedValue<JSObject> parentHandle;
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.parentHandle.castAndSetHermesValue<JSObject>(
      JSObject::create(
          runtime, Handle<JSObject>::vmcast(&runtime.iteratorPrototype))
          .getHermesValue());
  defineMethod(
      runtime,
      lv.parentHandle,
      Predefined::getSymbolID(Predefined::next),
      nullptr,
      mapIteratorPrototypeNext,
      0);

  auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  lv.str = runtime.getPredefinedString(Predefined::MapIterator);
  defineProperty(
      runtime,
      lv.parentHandle,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      lv.str,
      dpf);

  return lv.parentHandle.getHermesValue();
}

CallResult<HermesValue> mapIteratorPrototypeNext(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
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

/// ES15.0 24.1.2.1 Map.groupBy ( items, callback )
/// https://tc39.es/ecma262/#sec-map.groupby
CallResult<HermesValue> mapGroupBy(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  struct : public Locals {
    PinnedValue<JSMap> map;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 2. Let map be ! Construct(%Map%).
  lv.map = JSMap::create(runtime, runtime.mapPrototype);

  if (LLVM_UNLIKELY(
          JSMap::initializeStorage(lv.map, runtime) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 1. Let groups be ? GroupBy(items, callbackfn, COLLECTION).
  if (LLVM_UNLIKELY(
          groupByCollection(
              runtime, lv.map, args.getArgHandle(0), args.getArgHandle(1)) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // NOTE: The below are handled by `groupByCollection` which directly acts on
  // the above JSMap.
  // 2. Let map be ! Construct(%Map%).
  // 3. For each Record { [[Key]], [[Elements]] } g of groups, do
  // a. Let elements be CreateArrayFromList(g.[[Elements]]).
  // b. Let entry be the Record { [[Key]]: g.[[Key]], [[Value]]: elements }.
  // c. Append entry to map.[[MapData]].

  // 4. Return map.
  return lv.map.getHermesValue();
}

} // namespace vm
} // namespace hermes

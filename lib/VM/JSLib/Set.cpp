/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES6.0 23.2 Initialize the Set constructor.
//===----------------------------------------------------------------------===//

#include "JSLibInternal.h"

#include "hermes/VM/StringPrimitive.h"

namespace hermes {
namespace vm {

HermesValue createSetConstructor(Runtime &runtime) {
  auto setPrototype = Handle<JSObject>::vmcast(&runtime.setPrototype);

  struct : public Locals {
    PinnedValue<NativeConstructor> cons;
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Set.prototype.xxx methods.
  runtime.setPrototypeAdd = defineMethod(
      runtime,
      setPrototype,
      Predefined::getSymbolID(Predefined::add),
      nullptr,
      setPrototypeAdd,
      1);

  defineMethod(
      runtime,
      setPrototype,
      Predefined::getSymbolID(Predefined::clear),
      nullptr,
      setPrototypeClear,
      0);

  defineMethod(
      runtime,
      setPrototype,
      Predefined::getSymbolID(Predefined::deleteStr),
      nullptr,
      setPrototypeDelete,
      1);

  defineMethod(
      runtime,
      setPrototype,
      Predefined::getSymbolID(Predefined::entries),
      nullptr,
      setPrototypeEntries,
      0);

  defineMethod(
      runtime,
      setPrototype,
      Predefined::getSymbolID(Predefined::forEach),
      nullptr,
      setPrototypeForEach,
      1);

  runtime.setPrototypeHas = defineMethod(
      runtime,
      setPrototype,
      Predefined::getSymbolID(Predefined::has),
      nullptr,
      setPrototypeHas,
      1);

  defineAccessor(
      runtime,
      setPrototype,
      Predefined::getSymbolID(Predefined::size),
      nullptr,
      setPrototypeSizeGetter,
      nullptr,
      false,
      true);

  runtime.setPrototypeValues = defineMethod(
      runtime,
      setPrototype,
      Predefined::getSymbolID(Predefined::values),
      nullptr,
      setPrototypeValues,
      0);

  defineMethod(
      runtime,
      setPrototype,
      Predefined::getSymbolID(Predefined::difference),
      nullptr,
      setPrototypeDifference,
      1);

  defineMethod(
      runtime,
      setPrototype,
      Predefined::getSymbolID(Predefined::intersection),
      nullptr,
      setPrototypeIntersection,
      1);

  defineMethod(
      runtime,
      setPrototype,
      Predefined::getSymbolID(Predefined::isDisjointFrom),
      nullptr,
      setPrototypeIsDisjointFrom,
      1);

  defineMethod(
      runtime,
      setPrototype,
      Predefined::getSymbolID(Predefined::isSubsetOf),
      nullptr,
      setPrototypeIsSubsetOf,
      1);

  defineMethod(
      runtime,
      setPrototype,
      Predefined::getSymbolID(Predefined::isSupersetOf),
      nullptr,
      setPrototypeIsSupersetOf,
      1);

  defineMethod(
      runtime,
      setPrototype,
      Predefined::getSymbolID(Predefined::symmetricDifference),
      nullptr,
      setPrototypeSymmetricDifference,
      1);

  defineMethod(
      runtime,
      setPrototype,
      Predefined::getSymbolID(Predefined::unionStr),
      nullptr,
      setPrototypeUnion,
      1);

  DefinePropertyFlags dpf = DefinePropertyFlags::getNewNonEnumerableFlags();

  // Use the same valuesMethod for both keys() and values().
  runtime.ignoreAllocationFailure(
      JSObject::defineOwnProperty(
          setPrototype,
          runtime,
          Predefined::getSymbolID(Predefined::keys),
          dpf,
          runtime.setPrototypeValues));
  runtime.ignoreAllocationFailure(
      JSObject::defineOwnProperty(
          setPrototype,
          runtime,
          Predefined::getSymbolID(Predefined::SymbolIterator),
          dpf,
          runtime.setPrototypeValues));

  dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  lv.str = runtime.getPredefinedString(Predefined::Set);
  defineProperty(
      runtime,
      setPrototype,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      lv.str,
      dpf);

  defineSystemConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::Set),
      setConstructor,
      setPrototype,
      0,
      lv.cons);

  return lv.cons.getHermesValue();
}

/// Populate the Set with the contents of the source Set.
/// \param target the Set to populate (newly constructed).
/// \param src the Set to pull the entries from.
/// \return the newly populated Set.
static ExecutionStatus
setFromSetFastPath(Runtime &runtime, Handle<JSSet> target, Handle<JSSet> src) {
  // TODO: This can be improved further by avoiding any rehashes and the
  // SmallHermesValue unbox/boxing. We should be able to make an
  // OrderedHashMap::clone that initializes based on an existing Set
  // and clones all entries directly somehow.
  struct : public Locals {
    PinnedValue<> keyHandle;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  return JSSet::forEachNative(
      src,
      runtime,
      [&target, &lv](Runtime &runtime, HermesValue keyHV) -> ExecutionStatus {
        lv.keyHandle = keyHV;
        if (LLVM_UNLIKELY(
                JSSet::insert(target, runtime, lv.keyHandle) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        return ExecutionStatus::RETURNED;
      });
}

CallResult<HermesValue> setConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (LLVM_UNLIKELY(!args.isConstructorCall())) {
    return runtime.raiseTypeError("Constructor Set requires 'new'");
  }

  struct : public Locals {
    PinnedValue<JSObject> selfParent;
    PinnedValue<JSSet> self;
    PinnedValue<HermesValue> tmpHandle;
    PinnedValue<JSObject> tmpObjHandle;
    PinnedValue<Callable> adder;
    PinnedValue<> iterMethodHandle;
    PinnedValue<Callable> iterMethod;
    CheckedIteratorRecordStorage iterStorage;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  if (LLVM_LIKELY(
          args.getNewTarget().getRaw() ==
          runtime.setConstructor.getHermesValue().getRaw())) {
    lv.selfParent = runtime.setPrototype;
  } else {
    CallResult<PseudoHandle<JSObject>> thisParentRes =
        NativeConstructor::parentForNewThis_RJS(
            runtime,
            Handle<Callable>::vmcast(&args.getNewTarget()),
            runtime.setPrototype);
    if (LLVM_UNLIKELY(thisParentRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.selfParent = std::move(*thisParentRes);
  }
  lv.self = JSSet::create(runtime, lv.selfParent);

  if (LLVM_UNLIKELY(
          JSSet::initializeStorage(lv.self, runtime) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (args.getArgCount() == 0 || args.getArg(0).isUndefined() ||
      args.getArg(0).isNull()) {
    return lv.self.getHermesValue();
  }

  auto propRes = JSObject::getNamed_RJS(
      lv.self, runtime, Predefined::getSymbolID(Predefined::add));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // ES6.0 23.2.1.1.7: Cache adder across all iterations of the loop.
  auto *adder = dyn_vmcast<Callable>(propRes->getHermesValue());
  if (!adder) {
    return runtime.raiseTypeError("Property 'add' for Set is not callable");
  }
  lv.adder = adder;

  lv.iterMethodHandle = HermesValue::encodeSymbolValue(
      Predefined::getSymbolID(Predefined::SymbolIterator));
  auto iterMethodRes =
      getMethod(runtime, args.getArgHandle(0), lv.iterMethodHandle);
  if (LLVM_UNLIKELY(iterMethodRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (!vmisa<Callable>(iterMethodRes->getHermesValue())) {
    return runtime.raiseTypeError("iterator method is not callable");
  }
  lv.iterMethod = vmcast<Callable>(iterMethodRes->getHermesValue());

  // Fast path
  const bool originalAdd = lv.adder.getHermesValue().getRaw() ==
      runtime.setPrototypeAdd.getHermesValue().getRaw();
  // If the adder is the default one, we can call JSSet::insert directly.
  if (LLVM_LIKELY(originalAdd)) {
    // If the iterable is an array with unmodified iterator,
    // then we can do for-loop.
    if (Handle<JSArray> arr = args.dyncastArg<JSArray>(0); arr &&
        LLVM_LIKELY(lv.iterMethod.getHermesValue().getRaw() ==
                    runtime.arrayPrototypeValues.getHermesValue().getRaw())) {
      for (JSArray::size_type i = 0; i < JSArray::getLength(arr.get(), runtime);
           i++) {
        // Add each element to the set.
        auto element = arr.get()->at(runtime, i);
        if (LLVM_LIKELY(!element.isEmpty())) {
          lv.tmpHandle = element.unboxToHV(runtime);
          if (LLVM_UNLIKELY(
                  JSSet::insert(lv.self, runtime, lv.tmpHandle) ==
                  ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
        } else {
          CallResult<PseudoHandle<>> valueRes = getIndexed_RJS(runtime, arr, i);
          if (LLVM_UNLIKELY(valueRes == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }

          lv.tmpHandle = valueRes->getHermesValue();
          if (LLVM_UNLIKELY(
                  JSSet::insert(lv.self, runtime, lv.tmpHandle) ==
                  ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
        }
      }

      return lv.self.getHermesValue();
    }

    // If the iterable is a Set with an unmodified iterator,
    // then we can do for-loop.
    if (Handle<JSSet> inputSet = args.dyncastArg<JSSet>(0); inputSet &&
        LLVM_LIKELY(lv.iterMethod.getHermesValue().getRaw() ==
                    runtime.setPrototypeValues.getHermesValue().getRaw())) {
      if (LLVM_UNLIKELY(
              setFromSetFastPath(runtime, lv.self, inputSet) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      return lv.self.getHermesValue();
    }
  }
  // Slow path
  CallResult<CheckedIteratorRecord> iterRes = getCheckedIterator(
      runtime,
      args.getArgHandle(0),
      lv.iterStorage,
      Handle<Callable>{lv.iterMethod});
  if (LLVM_UNLIKELY(iterRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto iteratorRecord = *iterRes;

  // Iterate the array and add every element.

  // Check the length of the array after every iteration,
  // to allow for the fact that the length could be modified during iteration.
  for (;;) {
    CallResult<PseudoHandle<JSObject>> nextRes =
        iteratorStep(runtime, iteratorRecord);
    if (LLVM_UNLIKELY(nextRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (!*nextRes) {
      // Done with iteration.
      return lv.self.getHermesValue();
    }
    lv.tmpObjHandle = std::move(*nextRes);
    auto nextValueRes = JSObject::getNamed_RJS(
        lv.tmpObjHandle, runtime, Predefined::getSymbolID(Predefined::value));
    if (LLVM_UNLIKELY(nextValueRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    if (LLVM_UNLIKELY(
            Callable::executeCall1(
                lv.adder, runtime, lv.self, nextValueRes->get()) ==
            ExecutionStatus::EXCEPTION)) {
      return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
    }
  }
}

// ES12 23.2.3.1 Set.prototype.add ( value )
CallResult<HermesValue> setPrototypeAdd(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto selfHandle = args.dyncastThis<JSSet>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError("Non-Set object called on Set.prototype.add");
  }
  auto valueHandle = args.getArgHandle(0);
  // 5. If value is -0, set value to +0.
  // N.B. in the case of Set, the value is used as both the value and the key of
  // the entry. They are both observed as normalized from Set iterators.
  auto value = valueHandle->isNumber() && valueHandle->getNumber() == 0
      ? HandleRootOwner::getZeroValue()
      : valueHandle;
  if (LLVM_UNLIKELY(
          JSSet::insert(selfHandle, runtime, value) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return selfHandle.getHermesValue();
}

CallResult<HermesValue> setPrototypeClear(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto selfHandle = args.dyncastThis<JSSet>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Set object called on Set.prototype.clear");
  }
  if (LLVM_UNLIKELY(
          JSSet::clear(selfHandle, runtime) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue> setPrototypeDelete(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto selfHandle = args.dyncastThis<JSSet>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Set object called on Set.prototype.delete");
  }
  return HermesValue::encodeBoolValue(
      JSSet::erase(selfHandle, runtime, args.getArgHandle(0)));
}

CallResult<HermesValue> setPrototypeEntries(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto selfHandle = args.dyncastThis<JSSet>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Set object called on Set.prototype.entries");
  }
  struct : public Locals {
    PinnedValue<JSSetIterator> iterator;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.iterator = JSSetIterator::create(
      runtime, Handle<JSObject>::vmcast(&runtime.setIteratorPrototype));
  lv.iterator->initializeIterator(runtime, selfHandle, IterationKind::Entry);
  return lv.iterator.getHermesValue();
}

CallResult<HermesValue> setPrototypeForEach(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto selfHandle = args.dyncastThis<JSSet>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Set object called on Set.prototype.forEach");
  }
  auto callbackfn = args.dyncastArg<Callable>(0);
  if (LLVM_UNLIKELY(!callbackfn)) {
    return runtime.raiseTypeError(
        "callbackfn must be Callable inSet.prototype.forEach");
  }
  auto thisArg = args.getArgHandle(1);
  if (LLVM_UNLIKELY(
          JSSet::forEach(selfHandle, runtime, callbackfn, thisArg) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue> setPrototypeHas(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto *self = dyn_vmcast<JSSet>(args.getThisArg());
  if (LLVM_UNLIKELY(!self)) {
    return runtime.raiseTypeError("Non-Set object called on Set.prototype.has");
  }
  return HermesValue::encodeBoolValue(self->has(runtime, args.getArg(0)));
}

CallResult<HermesValue> setPrototypeSizeGetter(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto self = dyn_vmcast<JSSet>(args.getThisArg());
  if (LLVM_UNLIKELY(!self)) {
    return runtime.raiseTypeError(
        "Non-Set object called on Set.prototype.size");
  }
  return HermesValue::encodeTrustedNumberValue(self->size());
}

CallResult<HermesValue> setPrototypeValues(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto selfHandle = args.dyncastThis<JSSet>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Set object called on Set.prototype.values");
  }
  struct : public Locals {
    PinnedValue<JSSetIterator> iterator;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.iterator = JSSetIterator::create(
      runtime, Handle<JSObject>::vmcast(&runtime.setIteratorPrototype));
  lv.iterator->initializeIterator(runtime, selfHandle, IterationKind::Value);
  return lv.iterator.getHermesValue();
}

HermesValue createSetIteratorPrototype(Runtime &runtime) {
  struct : public Locals {
    PinnedValue<JSObject> parentHandle;
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.parentHandle = JSObject::create(
      runtime, Handle<JSObject>::vmcast(&runtime.iteratorPrototype));
  defineMethod(
      runtime,
      lv.parentHandle,
      Predefined::getSymbolID(Predefined::next),
      nullptr,
      setIteratorPrototypeNext,
      0);

  auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  lv.str = runtime.getPredefinedString(Predefined::SetIterator);
  defineProperty(
      runtime,
      lv.parentHandle,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      lv.str,
      dpf);

  return lv.parentHandle.getHermesValue();
}

CallResult<HermesValue> setIteratorPrototypeNext(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto selfHandle = args.dyncastThis<JSSetIterator>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-SetIterator object called on SetIterator.prototype.next");
  }
  auto cr = JSSetIterator::nextElement(selfHandle, runtime);
  if (cr == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return *cr;
}

// ES16 24.2.4.5 Set.prototype.difference(other)
CallResult<HermesValue> setPrototypeDifference(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. Let O be this value
  // 2. Perform ?RequireInternalSlot(O, [[SetData]])
  auto selfHandle = args.dyncastThis<JSSet>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Set `this` object called on Set.prototype.difference");
  }

  struct : Locals {
    PinnedValue<Callable> otherHasMethod;
    PinnedValue<Callable> otherKeysMethod;
    PinnedValue<JSSet> resultSet;
    PinnedValue<> tmp;
    CheckedIteratorRecordStorage iterStorage;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 3. Let otherRec be ?GetSetRecord(other)
  auto other = args.getArgHandle(0);
  double otherSize = 0;
  auto res = getSetRecord(
      runtime, other, &otherSize, &lv.otherHasMethod, &lv.otherKeysMethod);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 4. Let resultSetData be a copy of O.[[SetData]]
  lv.resultSet = JSSet::create(runtime, runtime.setPrototype);
  if (LLVM_UNLIKELY(
          JSSet::initializeStorage(lv.resultSet, runtime) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  uint32_t thisSize = selfHandle->size();
  // Possible fast-path scenarios when the other object is a Set:
  // 1. The size of `this` Set <= the size of other AND other.has is unmodified
  // 2. Else, the size of `this` Set > the size of other AND other.keys is
  // unmodified.
  // In these cases, the elements of the Set will not be modified,
  // and we can directly iterate over the Set to derive the resulting Set.
  if (auto otherSet = Handle<JSSet>::dyn_vmcast(other); LLVM_LIKELY(otherSet)) {
    bool fastPath = false;
    if (thisSize <= otherSize) {
      fastPath = lv.otherHasMethod.getHermesValue().getRaw() ==
          runtime.setPrototypeHas.getHermesValue().getRaw();
    } else {
      fastPath = lv.otherKeysMethod.getHermesValue().getRaw() ==
          runtime.setPrototypeValues.getHermesValue().getRaw();
    }
    if (LLVM_LIKELY(fastPath)) {
      auto forEachRes = JSSet::forEachNative(
          selfHandle,
          runtime,
          [&lv, &otherSet](Runtime &runtime, HermesValue keyHV) {
            lv.tmp = keyHV;
            if (!otherSet->has(runtime, *lv.tmp)) {
              if (LLVM_UNLIKELY(
                      JSSet::insert(lv.resultSet, runtime, lv.tmp) ==
                      ExecutionStatus::EXCEPTION)) {
                return ExecutionStatus::EXCEPTION;
              }
            }
            return ExecutionStatus::RETURNED;
          });
      if (LLVM_UNLIKELY(forEachRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      return lv.resultSet.getHermesValue();
    }
  }

  // Slow path
  if (LLVM_UNLIKELY(
          setFromSetFastPath(runtime, lv.resultSet, selfHandle) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  };

  if (thisSize <= otherSize) {
    // 5. If SetDataSize(O.[[SetData]]) <= otherRec.[[Size]])
    // a. Let thisSize be the number of elements in O.[[SetData]]
    // b. Let index be 0
    // c. Repeat while index < thisSize
    //   i. Let e be resultSetData[index]
    //   ii. If e is not EMPTY, then
    //   iii. Set index to index + 1
    auto forEachRes = JSSet::forEachNative(
        lv.resultSet,
        runtime,
        [&lv, &other](Runtime &runtime, HermesValue keyHV) {
          lv.tmp = keyHV;
          // ii.1. let inOther be ?ToBoolean(?Call(otherRec.[[Has]],
          // otherRec.[[SetObject]], e))
          auto hasRes = Callable::executeCall1(
              lv.otherHasMethod, runtime, other, *lv.tmp);
          if (LLVM_UNLIKELY(hasRes == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          // ii.2. If inOther is true, then set resultSetData[index] to EMPTY
          if (toBoolean(hasRes->get())) {
            JSSet::erase(lv.resultSet, runtime, lv.tmp);
          }
          return ExecutionStatus::RETURNED;
        });
    if (LLVM_UNLIKELY(forEachRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  } else {
    // 6. Else,
    // a. Let keysIter be ?GetIteratorFromMethod(otherRec.[[SetObject]],
    // otherRec.[[Keys]])
    // b. Let next be NOT-STARTED
    auto keysIterRes = getCheckedIterator(
        runtime,
        other,
        lv.iterStorage,
        llvh::Optional<Handle<Callable>>(lv.otherKeysMethod));
    if (LLVM_UNLIKELY(keysIterRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto keysIteratorRecord = *keysIterRes;
    // c. Repeat, while next is not DONE
    for (;;) {
      // i. Set next to ?IteratorStepValue(keysIters)
      auto stepValRes = iteratorStepValue(runtime, keysIteratorRecord, &lv.tmp);
      if (LLVM_UNLIKELY(stepValRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      // ii. If next is not DONE, then
      if (!*stepValRes) {
        break;
      }

      // 1. Set next to CanonicalizeKeyedCollectionKey(next)
      lv.tmp = canonicalizeKeyedCollectionKey(*lv.tmp);
      // 2. Let valueIndex be SetDataIndex(resultSetData, next)
      // 3. If valueIndex is not NOT-FOUND, then
      //   a. Set resultSetData[valueIndex] to EMPTY
      JSSet::erase(lv.resultSet, runtime, lv.tmp);
    }
  }
  // Step 7 and 8 are performed when we created our returned Set via
  // JSSet::create
  // 7. Let result be OrdinaryObjectCreate(%Set.prototype%, «[[SetData]]»)
  // 8. Set result.[[SetData]] to resultSetData

  // 9. Return result
  return lv.resultSet.getHermesValue();
}

// ES16 24.2.4.9 Set.prototype.intersection
CallResult<HermesValue> setPrototypeIntersection(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. Let O be the this value
  // 2. Perform ?RequireInternalSlot(O, [[SetData]])
  auto selfHandle = args.dyncastThis<JSSet>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Set `this` object called on Set.prototype.intersection");
  }
  struct : Locals {
    PinnedValue<Callable> otherHasMethod;
    PinnedValue<Callable> otherKeysMethod;
    PinnedValue<JSSet> resultSet;
    PinnedValue<> tmp;
    CheckedIteratorRecordStorage iterStorage;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  // 3. Let otherRec be ?GetSetRecord(other)
  double otherSize = 0;
  auto other = args.getArgHandle(0);
  auto otherRecRes = getSetRecord(
      runtime, other, &otherSize, &lv.otherHasMethod, &lv.otherKeysMethod);
  if (LLVM_UNLIKELY(otherRecRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 4. Let resultSetData be a new emptyList
  lv.resultSet = JSSet::create(runtime, runtime.setPrototype);
  if (LLVM_UNLIKELY(
          JSSet::initializeStorage(lv.resultSet, runtime) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto thisSize = selfHandle->size();

  // When the other object is Set, it is possible to have a fast-path where the
  // elements of the Set will not be modified, and we can directly iterate over
  // the set to derive the resulting Set.
  // Note the order of the resulting Set is consistent with the smaller set.
  if (auto otherSet = Handle<JSSet>::dyn_vmcast(other); LLVM_LIKELY(otherSet)) {
    const auto &setToIterate = thisSize <= otherSize ? selfHandle : otherSet;
    const auto &setToCheck = thisSize <= otherSize ? otherSet : selfHandle;
    bool fastPath = false;
    // Fast-path 1: If the size of `this` Set <= the size of other AND
    // other.has is unmodified.
    if (thisSize <= otherSize) {
      fastPath = lv.otherHasMethod.getHermesValue().getRaw() ==
          runtime.setPrototypeHas.getHermesValue().getRaw();
    } else {
      // Fast-path 2: The size of `this` Set > the size of other AND other.keys
      // is unmodified
      fastPath = lv.otherKeysMethod.getHermesValue().getRaw() ==
          runtime.setPrototypeValues.getHermesValue().getRaw();
    }

    if (LLVM_LIKELY(fastPath)) {
      auto forEachRes = JSSet::forEachNative(
          setToIterate,
          runtime,
          [&lv, &setToCheck](Runtime &runtime, HermesValue keyHV) {
            lv.tmp = keyHV;
            if (setToCheck->has(runtime, *lv.tmp)) {
              if (LLVM_UNLIKELY(
                      JSSet::insert(lv.resultSet, runtime, lv.tmp) ==
                      ExecutionStatus::EXCEPTION)) {
                return ExecutionStatus::EXCEPTION;
              }
            }
            return ExecutionStatus::RETURNED;
          });
      if (LLVM_UNLIKELY(forEachRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      return lv.resultSet.getHermesValue();
    }
  }

  if (thisSize <= otherSize) {
    // 5. If SetDataSize(O.[[SetData]]) <= otherRec.[[Size]]
    // a. Let thisSize be the number of elements in O.[[SetData]]
    // b. Let index be 0
    // c. Repeat while index < thisSize
    //   i. Let e be O.[[SetData]][index]
    //   ii. Set index to index + 1
    //   iii. If e is not EMPTY, then
    auto forEachRes = JSSet::forEachNative(
        selfHandle,
        runtime,
        [&lv, &other](Runtime &runtime, HermesValue keyHV) {
          lv.tmp = keyHV;
          // 1. let inOther be ?ToBoolean(?Call(otherRec.[[Has]],
          // otherRec.[[SetObject]], e))
          auto hasRes = Callable::executeCall1(
              lv.otherHasMethod, runtime, other, *lv.tmp);
          if (LLVM_UNLIKELY(hasRes == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          if (toBoolean(hasRes->get())) {
            // 2. If inOther is true, then
            // b. If SetDataHas(resultSetData, e) is false, then append e to
            // resultSetData
            // JSSet::insert will only insert if the element doesn't
            // already exist,so we can skip the has check in step b.
            if (LLVM_UNLIKELY(
                    JSSet::insert(lv.resultSet, runtime, lv.tmp) ==
                    ExecutionStatus::EXCEPTION)) {
              return ExecutionStatus::EXCEPTION;
            }
          }
          // Using JSSet::forEachNative will handle any potential modifications
          // of the set during iteration, which can occur from calling
          // otherRec.[[Has]]
          // This covers the next steps in the spec:
          // 3. NOTE: The number of elements in O.[[SetData]] may have increased
          // during execution of otherRec.[[Has]]
          // 4. Let thisSize to the number of element in O.[[SetData]]
          return ExecutionStatus::RETURNED;
        });
    if (LLVM_UNLIKELY(forEachRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  } else {
    // 6. Else,
    // a. Let keysIter be ?GetIteratorFromMethod(otherRec.[[SetObject]],
    // otherRec.[[Keys]])
    auto keysIterRes = getCheckedIterator(
        runtime,
        other,
        lv.iterStorage,
        llvh::Optional<Handle<Callable>>(lv.otherKeysMethod));
    if (LLVM_UNLIKELY(keysIterRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto keysIterRecord = *keysIterRes;
    // c. Repeat while next is not DONE
    for (;;) {
      // i. Set next to ?IteratorStepValue(keysIter)
      auto stepValRes = iteratorStepValue(runtime, keysIterRecord, &lv.tmp);
      if (LLVM_UNLIKELY(stepValRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      // ii. If next is not DONE, then
      if (!*stepValRes) {
        break;
      }

      // 1. Set next to CanonicalizeKeyedCollectionKey(next)
      lv.tmp = canonicalizeKeyedCollectionKey(*lv.tmp);
      // 2. Let inThis be SetDataHas(O.[[SetData]], next)
      // 3. If inThis is true, then
      if (selfHandle->has(runtime, *lv.tmp)) {
        // b. If SetDataHas(resultSetData, next) is false, then
        //   i. Append next to resultSetData
        // JSSet::insert will only insert if the element doesn't already exist,
        // so we skip the has check in step b.
        if (LLVM_UNLIKELY(
                JSSet::insert(lv.resultSet, runtime, lv.tmp) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
      }
    }
  }
  return lv.resultSet.getHermesValue();
}

// ES16 24.2.4.10 Set.prototype.isDisjointFrom
CallResult<HermesValue> setPrototypeIsDisjointFrom(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. Let O be the this value
  // 2. Perform ?RequireInternalSlot(0, [[SetData]])
  auto selfHandle = args.dyncastThis<JSSet>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-Set `this` object called on Set.prototype.isDisjointFrom");
  }

  struct : Locals {
    PinnedValue<Callable> otherHasMethod;
    PinnedValue<Callable> otherKeysMethod;
    PinnedValue<> tmp;
    CheckedIteratorRecordStorage iterStorage;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 3. Let otherRec be ?GetSetRecord(other)
  double otherSize = 0;
  auto other = args.getArgHandle(0);
  auto otherRecRes = getSetRecord(
      runtime, other, &otherSize, &lv.otherHasMethod, &lv.otherKeysMethod);
  if (otherRecRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  auto thisSize = selfHandle->size();

  // Possible fast-path scenarios when the other object is a Set:
  // 1. The size of `this` Set <= the size of other AND other.has is unmodified
  // 2. Else, the size of `this` Set > the size of other AND other.keys is
  // unmodified.
  // In these cases, the elements of the Set will not be modified,
  // and we can directly iterate over the Set to derive the result.
  if (auto *otherSet = dyn_vmcast<JSSet>(*other); LLVM_LIKELY(otherSet)) {
    NoAllocScope noAlloc{runtime};

    bool fastPath = false;
    if (thisSize <= otherSize) {
      fastPath = lv.otherHasMethod.getHermesValue().getRaw() ==
          runtime.setPrototypeHas.getHermesValue().getRaw();
    } else {
      fastPath = lv.otherKeysMethod.getHermesValue().getRaw() ==
          runtime.setPrototypeValues.getHermesValue().getRaw();
    }
    if (LLVM_LIKELY(fastPath)) {
      JSSet *setToIterate = thisSize <= otherSize ? *selfHandle : otherSet;
      JSSet *setToCheck = thisSize <= otherSize ? otherSet : *selfHandle;
      JSSet::IteratorContext iterCtx = setToIterate->newIterator(runtime);
      while (setToIterate->advanceIterator(runtime, iterCtx)) {
        if (setToCheck->has(
                runtime, setToIterate->iteratorKey(runtime, iterCtx))) {
          return HermesValue::encodeBoolValue(false);
        }
      }
      return HermesValue::encodeBoolValue(true);
    }
  }
  if (thisSize <= otherSize) {
    // 4. If SetDataSize(O.[[SetData]]) <= otherRec.[[Size]], then
    // a. Let thisSize be the number of elements in O.[[SetData]]
    // b. Let index be 0
    // c. Repeat, while index < thisSize
    //   i. Let e be O.[[SetData]][index]
    //   ii. Set index to index + 1
    //   iii. If e is not EMPTY, then
    JSSet::IteratorContext iterCtx = selfHandle->newIterator(runtime);
    while (selfHandle->advanceIterator(runtime, iterCtx)) {
      // 1. Let inOther be ToBoolean(?Call(otherRec.[[Has]],
      //    otherRec.[[SetObject]], e))
      auto hasRes = Callable::executeCall1(
          lv.otherHasMethod,
          runtime,
          other,
          selfHandle->iteratorKey(runtime, iterCtx));
      if (LLVM_UNLIKELY(hasRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      // 2. If inOther is true, then return false
      if (toBoolean(hasRes->get())) {
        return HermesValue::encodeBoolValue(false);
      }
    }
  } else {
    // 5. Else,
    // a. Let keysIter be ?GetIteratorFromMethod(otherRec.[[SetObject]],
    // otherRec.[[Keys]])
    auto keysIterRes = getCheckedIterator(
        runtime,
        other,
        lv.iterStorage,
        llvh::Optional<Handle<Callable>>(lv.otherKeysMethod));
    if (LLVM_UNLIKELY(keysIterRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto keysIterRecord = *keysIterRes;
    // b. Let next be NOT-STARTED
    // c. Repeat, while next is not DONE,
    for (;;) {
      // i. Set next to ?IteratorStepValue(keysIter)
      auto stepValRes = iteratorStepValue(runtime, keysIterRecord, &lv.tmp);
      if (LLVM_UNLIKELY(stepValRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      // ii. If next is not DONE, then
      if (!*stepValRes) {
        break;
      }

      // 1. If SetDataHas(O.[[SetData]], next) is true, then
      if (selfHandle->has(runtime, *lv.tmp)) {
        // a. Perform ?IteratorClose(keysIters, NormalCompletion(UNUSED))
        auto closeRes = iteratorClose(
            runtime, keysIterRecord.iterator, Runtime::getEmptyValue());
        if (LLVM_UNLIKELY(closeRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        // b. Return false
        return HermesValue::encodeBoolValue(false);
      }
    }
  }
  // 6. Return true
  return HermesValue::encodeBoolValue(true);
}

// ES16 24.2.4.11 Set.prototype.isSubsetOf
CallResult<HermesValue> setPrototypeIsSubsetOf(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // Let O be the this value
  // Perform ?RequireInternalSlot(O, [[SetData]])
  auto selfHandle = args.dyncastThis<JSSet>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-set `this` object called on Set.prototypee.isSubsetOf");
  }
  struct : Locals {
    PinnedValue<Callable> otherHasMethod;
    PinnedValue<Callable> otherKeysMethod;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 3. Let otherRec be ?GetSetRecord(other)
  double otherSize = 0;
  auto other = args.getArgHandle(0);
  auto otherRecRes = getSetRecord(
      runtime, other, &otherSize, &lv.otherHasMethod, &lv.otherKeysMethod);
  if (LLVM_UNLIKELY(otherRecRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  auto thisSize = selfHandle->size();
  // 4. If SetDataSize(O.[[SetData]]) > otherRec.[[Size]], return false
  if (thisSize > otherSize) {
    return HermesValue::encodeBoolValue(false);
  }

  // 5. Let thisSize be the number of elements in O.[[SetData]]
  // 6. Let index be 0
  // 7. Repeat, while index < thisSize
  //   a. Let e be O.[[SetData]][index]
  //   b. Set index to index + 1
  //   c. If e is not EMPTY, then
  JSSet::IteratorContext iterCtx = selfHandle->newIterator(runtime);
  while (selfHandle->advanceIterator(runtime, iterCtx)) {
    // i. Let inOther be ToBoolean(?Call(otherRec.[[Has]],
    //    otherRec.[[SetObject]], e))
    auto hasRes = Callable::executeCall1(
        lv.otherHasMethod,
        runtime,
        other,
        selfHandle->iteratorKey(runtime, iterCtx));
    if (LLVM_UNLIKELY(hasRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // ii. If inOther is false, return false
    if (!toBoolean(hasRes->get())) {
      return HermesValue::encodeBoolValue(false);
    }
  }

  // 8. Return true
  return HermesValue::encodeBoolValue(true);
}

// ES16 24.2.4.12
CallResult<HermesValue> setPrototypeIsSupersetOf(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. Let O be the this value
  // 2. Perform ?RequireInternalSlot(O, [[SetData]])
  auto selfHandle = args.dyncastThis<JSSet>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-set `this` object called on Set.prototype.isSupersetOf");
  }
  struct : Locals {
    PinnedValue<Callable> otherHasMethod;
    PinnedValue<Callable> otherKeysMethod;
    PinnedValue<> tmp;
    CheckedIteratorRecordStorage iterStorage;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 3. Let otherRec be ?GetSetRecord(other)
  double otherSize = 0;
  auto other = args.getArgHandle(0);
  auto otherRecRes = getSetRecord(
      runtime, other, &otherSize, &lv.otherHasMethod, &lv.otherKeysMethod);
  if (LLVM_UNLIKELY(otherRecRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 4. If SetDataSize(O.[[SetData]]) < otherRec.[[Size]], return false
  auto thisSize = selfHandle->size();
  if (thisSize < otherSize) {
    return HermesValue::encodeBoolValue(false);
  }

  // Fast-path: If the other object is a Set and its keys property is
  // unmodified, then the elements of the Set will not be modified. We can
  // directly iterate over the other Set to check if it is in the `this` Set.
  auto *otherSet = dyn_vmcast<JSSet>(*other);
  const bool originalKeys = lv.otherKeysMethod.getHermesValue().getRaw() ==
      runtime.setPrototypeValues.getHermesValue().getRaw();
  if (LLVM_LIKELY(otherSet && originalKeys)) {
    NoAllocScope noAlloc{runtime};
    JSSet::IteratorContext iterCtx = otherSet->newIterator(runtime);
    while (otherSet->advanceIterator(runtime, iterCtx)) {
      if (!selfHandle->has(runtime, otherSet->iteratorKey(runtime, iterCtx))) {
        return HermesValue::encodeBoolValue(false);
      }
    }
    return HermesValue::encodeBoolValue(true);
  }

  // 5. Let keysIter be ?GetIteratorFromMethod(otherRec.[[SetObject]],
  // otherRec.[[Keys]])
  auto keysIterRes = getCheckedIterator(
      runtime,
      other,
      lv.iterStorage,
      llvh::Optional<Handle<Callable>>(lv.otherKeysMethod));
  if (LLVM_UNLIKELY(keysIterRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto keysIterRecord = *keysIterRes;
  // 6. Let next be NON-STARTED
  // 7. Repeat, while next is not DONE
  for (;;) {
    // a. Set next to ?IteratorStepValue(keysIter)
    auto stepValRes = iteratorStepValue(runtime, keysIterRecord, &lv.tmp);
    if (LLVM_UNLIKELY(stepValRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // b. If next is not DONE, then
    if (!*stepValRes) {
      break;
    }

    // i. If SetDataHas(O.[[SetData]], next) is false, then
    if (!selfHandle->has(runtime, *lv.tmp)) {
      // 1. Perform ?IteratorClose(keysIter, NormalCompletion(UNUSED))
      auto closeRes = iteratorClose(
          runtime, keysIterRecord.iterator, Runtime::getEmptyValue());
      if (LLVM_UNLIKELY(closeRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      // 2. Return false
      return HermesValue::encodeBoolValue(false);
    }
  }
  // 8. Return true
  return HermesValue::encodeBoolValue(true);
}

// ES16 24.2.4.15 Set.prototype.symmetricDifference
CallResult<HermesValue> setPrototypeSymmetricDifference(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. Let O be the this value
  // 2. Perform ?RequireInternalSlot(O, [[SetData]])
  auto selfHandle = args.dyncastThis<JSSet>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-set `this` object called on Set.prototype.symmetricDifference");
  }
  struct : Locals {
    PinnedValue<Callable> otherHasMethod;
    PinnedValue<Callable> otherKeysMethod;
    PinnedValue<JSSet> resultSet;
    PinnedValue<> tmp;
    CheckedIteratorRecordStorage iterStorage;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 3. Let otherRec be ?GetSetRecord(other)
  double otherSize = 0;
  auto other = args.getArgHandle(0);
  auto otherRecRes = getSetRecord(
      runtime, other, &otherSize, &lv.otherHasMethod, &lv.otherKeysMethod);
  if (LLVM_UNLIKELY(otherRecRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  lv.resultSet = JSSet::create(runtime, runtime.setPrototype);
  if (LLVM_UNLIKELY(
          JSSet::initializeStorage(lv.resultSet, runtime) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // Fast-path: If the other object is a Set and its keys property is
  // unmodified, then the elements of the Set will not be modified. We can
  // directly iterate over the other Set to derive our resulting Set.
  auto otherSet = Handle<JSSet>::dyn_vmcast(other);
  const bool originalKeys = lv.otherKeysMethod.getHermesValue().getRaw() ==
      runtime.setPrototypeValues.getHermesValue().getRaw();
  if (LLVM_LIKELY(otherSet && originalKeys)) {
    if (LLVM_UNLIKELY(
            setFromSetFastPath(runtime, lv.resultSet, selfHandle) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto forEachRes = JSSet::forEachNative(
        otherSet,
        runtime,
        [&lv, &selfHandle](Runtime &runtime, HermesValue keyHV) {
          lv.tmp = keyHV;
          if (selfHandle->has(runtime, *lv.tmp)) {
            JSSet::erase(lv.resultSet, runtime, lv.tmp);
          } else {
            if (LLVM_UNLIKELY(
                    JSSet::insert(lv.resultSet, runtime, lv.tmp) ==
                    ExecutionStatus::EXCEPTION)) {
              return ExecutionStatus::EXCEPTION;
            }
          }
          return ExecutionStatus::RETURNED;
        });
    if (LLVM_UNLIKELY(forEachRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return lv.resultSet.getHermesValue();
  }

  // 4. Let keysIters be ?GetIteratorFromMethod(otherRec.[[SetObject]],
  // otherRec.[[Keys]])
  auto keysIterRes = getCheckedIterator(
      runtime,
      other,
      lv.iterStorage,
      llvh::Optional<Handle<Callable>>(lv.otherKeysMethod));
  if (LLVM_UNLIKELY(keysIterRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto keysIterRecord = *keysIterRes;

  // 5. Let resultSetData be a copy of O.[[SetData]]
  if (LLVM_UNLIKELY(
          setFromSetFastPath(runtime, lv.resultSet, selfHandle) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 6. Let next be NON-STARTED
  // 7. Repeat, while next is not DONE,
  for (;;) {
    // a. Set next to ?IteratorStepValue(keysIter)
    auto stepValRes = iteratorStepValue(runtime, keysIterRecord, &lv.tmp);
    if (LLVM_UNLIKELY(stepValRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // b. If next is not DONE, then
    if (!*stepValRes) {
      break;
    }

    // i. Set next to CanonicalizeKeyedCollection(next)
    lv.tmp = canonicalizeKeyedCollectionKey(*lv.tmp);
    // ii. Let resultIndex be SetDataIndex(resultSetData, next)
    // iii. If resultIndex is NOT-FOUND, let alreadyInResult be false. Otherwise
    //      let alreadyInResult be true.
    auto alreadyInResult = lv.resultSet->has(runtime, *lv.tmp);

    // iv. If SetDataHas(O.[[SetData]], next) is true, then
    // v. Else,
    if (selfHandle->has(runtime, *lv.tmp)) {
      // 1. If alreadyInResult is true, then set resultSetData[resultIndex] to
      // EMPTY
      if (alreadyInResult) {
        JSSet::erase(lv.resultSet, runtime, lv.tmp);
      }
    } else {
      // If alreadyInResult is false, append next to resultSetData
      if (!alreadyInResult) {
        if (LLVM_UNLIKELY(
                JSSet::insert(lv.resultSet, runtime, lv.tmp) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
      }
    }
  }

  // Step 8 and 9 are performed when we created our returned Set via
  // JSSet::create
  // 8. Let result be OrdinaryObjectCreate(%Set.prototype%, «[[SetData]]»)
  // 9. Set result.[[SetData]] to resultSetData
  return lv.resultSet.getHermesValue();
}

// ES16 24.2.4.16 Set.prototype.union
CallResult<HermesValue> setPrototypeUnion(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. Let O be the this value
  // 2. Perform ?RequireInternalSlot(O, [[SetData]])
  auto selfHandle = args.dyncastThis<JSSet>();
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime.raiseTypeError(
        "Non-set `this` object called on Set.prototype.union");
  }
  struct : Locals {
    PinnedValue<Callable> otherHasMethod;
    PinnedValue<Callable> otherKeysMethod;
    PinnedValue<JSSet> resultSet;
    PinnedValue<> tmp;
    CheckedIteratorRecordStorage iterStorage;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 3. Let otherRec be ?GetSetRecord(other)
  double otherSize = 0;
  auto other = args.getArgHandle(0);
  auto otherRecRes = getSetRecord(
      runtime, other, &otherSize, &lv.otherHasMethod, &lv.otherKeysMethod);
  if (LLVM_UNLIKELY(otherRecRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  lv.resultSet = JSSet::create(runtime, runtime.setPrototype);
  if (LLVM_UNLIKELY(
          JSSet::initializeStorage(lv.resultSet, runtime) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // Fast-path: If the other object is a Set and its keys property is
  // unmodified, then the elements of the Set will not be modified. We can
  // directly iterate over the other Set to derive our resulting Set.
  auto otherSet = Handle<JSSet>::dyn_vmcast(other);
  const bool originalKeys = lv.otherKeysMethod.getHermesValue().getRaw() ==
      runtime.setPrototypeValues.getHermesValue().getRaw();
  if (LLVM_LIKELY(otherSet && originalKeys)) {
    if (LLVM_UNLIKELY(
            setFromSetFastPath(runtime, lv.resultSet, selfHandle) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto forEachRes = JSSet::forEachNative(
        otherSet, runtime, [&lv](Runtime &runtime, HermesValue keyHV) {
          lv.tmp = keyHV;
          if (LLVM_UNLIKELY(
                  JSSet::insert(lv.resultSet, runtime, lv.tmp) ==
                  ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          return ExecutionStatus::RETURNED;
        });
    if (LLVM_UNLIKELY(forEachRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return lv.resultSet.getHermesValue();
  }

  // 4. Let keysIter be ?GetIteratorFromMethod(otherRec.[[SetObject]],
  // otherRec.[[Keys]])
  auto keysIterRes = getCheckedIterator(
      runtime,
      other,
      lv.iterStorage,
      llvh::Optional<Handle<Callable>>(lv.otherKeysMethod));
  if (LLVM_UNLIKELY(keysIterRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto keysIterRecord = *keysIterRes;

  // 5. Let resultSetData be a copy of O.[[SetData]]
  if (LLVM_UNLIKELY(
          setFromSetFastPath(runtime, lv.resultSet, selfHandle) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 6. Let next be NOT-STARTED
  // 7. Repeat, while next is not DONE,
  for (;;) {
    // a. Set next to ?IteratorStepValue(keysIter)
    auto stepValRes = iteratorStepValue(runtime, keysIterRecord, &lv.tmp);
    if (LLVM_UNLIKELY(stepValRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // b. If next is not DONE, then
    if (!*stepValRes) {
      break;
    }
    // i. Set next to CanonicalizeKeyedCollectionKey(next)
    lv.tmp = canonicalizeKeyedCollectionKey(*lv.tmp);
    // ii. If SetDataHas(resultSetData, next) is false, then
    //   1. Append next to resultSetData
    // JSSet::insert will only insert if the element doesn't already exist,
    // so we can skip the has check in step b.
    if (LLVM_UNLIKELY(
            JSSet::insert(lv.resultSet, runtime, lv.tmp) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }

  // Step 8 and 9 are performed when we created our returned Set via
  // JSSet::create
  // 8. Let result be OrdinaryObjectCreate(%Set.prototype%, «[[SetData]]»)
  // 9. Set result.[[SetData]] to resultSetData
  return lv.resultSet.getHermesValue();
}
} // namespace vm
} // namespace hermes

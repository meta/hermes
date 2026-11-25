/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES5.1 15.2 Initialize the Object constructor.
//===----------------------------------------------------------------------===//
#include "Object.h"
#include "JSLibInternal.h"

#include "hermes/VM/ArrayStorage.h"
#include "hermes/VM/FastArray.h"
#include "hermes/VM/HermesValueTraits.h"
#include "hermes/VM/JSLib.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/PrimitiveBox.h"
#include "hermes/VM/PropertyAccessor.h"
#include "hermes/VM/StringBuilder.h"

namespace hermes {
namespace vm {

//===----------------------------------------------------------------------===//
/// Object.

HermesValue createObjectConstructor(Runtime &runtime) {
  assert(runtime.objectPrototype.get() != nullptr);
  auto objectPrototype = Handle<JSObject>::vmcast(&runtime.objectPrototype);

  struct : public Locals {
    PinnedValue<NativeConstructor> cons;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  defineSystemConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::Object),
      objectConstructor,
      Handle<JSObject>::vmcast(&runtime.objectPrototype),
      1,
      lv.cons);
  void *ctx = nullptr;

  // Object.prototype.xxx methods.
  defineMethod(
      runtime,
      objectPrototype,
      Predefined::getSymbolID(Predefined::toString),
      ctx,
      objectPrototypeToString,
      0);
  defineMethod(
      runtime,
      objectPrototype,
      Predefined::getSymbolID(Predefined::toLocaleString),
      ctx,
      objectPrototypeToLocaleString,
      0);
  defineMethod(
      runtime,
      objectPrototype,
      Predefined::getSymbolID(Predefined::valueOf),
      ctx,
      objectPrototypeValueOf,
      0);
  defineMethod(
      runtime,
      objectPrototype,
      Predefined::getSymbolID(Predefined::hasOwnProperty),
      ctx,
      objectPrototypeHasOwnProperty,
      1);
  defineMethod(
      runtime,
      objectPrototype,
      Predefined::getSymbolID(Predefined::isPrototypeOf),
      ctx,
      objectPrototypeIsPrototypeOf,
      1);
  defineMethod(
      runtime,
      objectPrototype,
      Predefined::getSymbolID(Predefined::propertyIsEnumerable),
      ctx,
      objectPrototypePropertyIsEnumerable,
      1);
  defineAccessor(
      runtime,
      objectPrototype,
      Predefined::getSymbolID(Predefined::underscore_proto),
      ctx,
      objectPrototypeProto_getter,
      objectPrototypeProto_setter,
      false,
      true);
  defineMethod(
      runtime,
      objectPrototype,
      Predefined::getSymbolID(Predefined::__defineGetter__),
      ctx,
      objectPrototypeDefineGetter,
      2);
  defineMethod(
      runtime,
      objectPrototype,
      Predefined::getSymbolID(Predefined::__defineSetter__),
      ctx,
      objectPrototypeDefineSetter,
      2);
  defineMethod(
      runtime,
      objectPrototype,
      Predefined::getSymbolID(Predefined::__lookupGetter__),
      ctx,
      objectPrototypeLookupGetter,
      1);
  defineMethod(
      runtime,
      objectPrototype,
      Predefined::getSymbolID(Predefined::__lookupSetter__),
      ctx,
      objectPrototypeLookupSetter,
      1);

  // Object.xxx() methods.
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::getPrototypeOf),
      ctx,
      objectGetPrototypeOf,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::getOwnPropertyDescriptor),
      ctx,
      objectGetOwnPropertyDescriptor,
      2);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::getOwnPropertyDescriptors),
      ctx,
      objectGetOwnPropertyDescriptors,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::getOwnPropertyNames),
      ctx,
      objectGetOwnPropertyNames,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::getOwnPropertySymbols),
      ctx,
      objectGetOwnPropertySymbols,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::groupBy),
      ctx,
      objectGroupBy,
      2);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::hasOwn),
      ctx,
      objectHasOwn,
      2);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::seal),
      ctx,
      objectSeal,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::freeze),
      ctx,
      objectFreeze,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::fromEntries),
      ctx,
      objectFromEntries,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::preventExtensions),
      ctx,
      objectPreventExtensions,
      1);

  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::is),
      ctx,
      objectIs,
      2);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::isSealed),
      ctx,
      objectIsSealed,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::isFrozen),
      ctx,
      objectIsFrozen,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::isExtensible),
      ctx,
      objectIsExtensible,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::keys),
      ctx,
      objectKeys,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::values),
      ctx,
      objectValues,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::entries),
      ctx,
      objectEntries,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::create),
      ctx,
      objectCreate,
      2);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::defineProperty),
      ctx,
      objectDefineProperty,
      3);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::defineProperties),
      ctx,
      objectDefineProperties,
      2);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::assign),
      ctx,
      objectAssign,
      2);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::setPrototypeOf),
      ctx,
      objectSetPrototypeOf,
      2);

  return lv.cons.getHermesValue();
}

/// ES2024 20.1.1.1 Object constructor.
CallResult<HermesValue> objectConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto &newTarget = args.getNewTarget();
  // 1. If NewTarget is neither undefined nor the active function object, then
  if (!newTarget.isUndefined() &&
      newTarget.getRaw() !=
          runtime.objectConstructor.getHermesValue().getRaw()) {
    // a. Return ? OrdinaryCreateFromConstructor(NewTarget,
    // "%Object.prototype%").
    auto res = ordinaryCreateFromConstructor_RJS(
        runtime,
        Handle<Callable>::vmcast(&args.getNewTarget()),
        runtime.objectPrototype);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return res->getHermesValue();
  }

  auto value = args.getArgHandle(0);
  // 2. If value is either undefined or null, return
  // OrdinaryObjectCreate(%Object.prototype%).
  if (value->isUndefined() || value->isNull()) {
    return JSObject::create(runtime).getHermesValue();
  }

  // 3. Return ! ToObject(value).
  return toObject(runtime, value);
}

CallResult<HermesValue> getPrototypeOf(Runtime &runtime, Handle<JSObject> obj) {
  CallResult<PseudoHandle<JSObject>> protoRes =
      JSObject::getPrototypeOf(obj, runtime);
  if (LLVM_UNLIKELY(protoRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // Note that we must return 'null' if there is no prototype.
  if (!*protoRes) {
    return HermesValue::encodeNullValue();
  }
  return protoRes->getHermesValue();
}

CallResult<HermesValue> objectGetPrototypeOf(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  CallResult<HermesValue> res = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  struct : public Locals {
    PinnedValue<JSObject> O;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.O.castAndSetHermesValue<JSObject>(*res);
  return getPrototypeOf(runtime, lv.O);
}

CallResult<HermesValue> getOwnPropertyDescriptor(
    Runtime &runtime,
    Handle<JSObject> object,
    Handle<> key) {
  ComputedPropertyDescriptor desc;
  struct : public Locals {
    PinnedValue<> valueOrAccessor;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  {
    auto result = JSObject::getOwnComputedDescriptor(
        object, runtime, key, desc, lv.valueOrAccessor);
    if (result == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    if (!*result) {
      if (LLVM_LIKELY(!object->isHostObject(runtime)))
        return HermesValue::encodeUndefinedValue();
      // For compatibility with polyfills we want to pretend that all HostObject
      // properties are "own" properties in hasOwnProperty() and in
      // getOwnPropertyDescriptor(). Since there is no way to check for a
      // HostObject property, we must always assume the property exists.
      desc.flags.enumerable = true;
      desc.flags.writable = true;
      desc.flags.hostObject = true;
    }
  }

  if (LLVM_UNLIKELY(!desc.flags.accessor && desc.flags.hostObject)) {
    auto propRes = JSObject::getComputed_RJS(object, runtime, key);
    if (propRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.valueOrAccessor = std::move(*propRes);
  }

  DefinePropertyFlags dpFlags;
  dpFlags.configurable = desc.flags.configurable;
  dpFlags.enumerable = desc.flags.enumerable;
  dpFlags.writable = desc.flags.writable;
  dpFlags.setEnumerable = true;
  dpFlags.setWritable = true;
  dpFlags.setConfigurable = true;
  dpFlags.setGetter = desc.flags.accessor;
  dpFlags.setSetter = desc.flags.accessor;
  dpFlags.setValue = !desc.flags.accessor;

  return objectFromPropertyDescriptor(runtime, dpFlags, lv.valueOrAccessor);
}

CallResult<HermesValue> objectGetOwnPropertyDescriptor(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto objRes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<JSObject> O;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.O.castAndSetHermesValue<JSObject>(objRes.getValue());

  return getOwnPropertyDescriptor(runtime, lv.O, args.getArgHandle(1));
}

/// ES10.0 19.1.2.9
CallResult<HermesValue> objectGetOwnPropertyDescriptors(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  // 1. Let obj be ? ToObject(O).
  auto objRes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<JSObject> obj;
    PinnedValue<JSObject> descriptors;
    PinnedValue<> key;
    PinnedValue<> descriptor;
    PinnedValue<JSArray> ownKeys;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.obj.castAndSetHermesValue<JSObject>(objRes.getValue());

  // 2. Let ownKeys be ? obj.[[OwnPropertyKeys]]().
  auto ownKeysRes = JSObject::getOwnPropertyKeys(
      lv.obj,
      runtime,
      OwnKeysFlags()
          .plusIncludeNonSymbols()
          .plusIncludeSymbols()
          .plusIncludeNonEnumerable());
  if (LLVM_UNLIKELY(ownKeysRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.ownKeys = std::move(*ownKeysRes);
  uint32_t len = JSArray::getLength(*lv.ownKeys, runtime);

  // 3. Let descriptors be ! ObjectCreate(%ObjectPrototype%).
  lv.descriptors = JSObject::create(runtime);

  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();

  // 4. For each element key of ownKeys in List order, do
  for (uint32_t i = 0; i < len; ++i) {
    lv.key = lv.ownKeys->at(runtime, i).unboxToHV(runtime);
    // a. Let desc be ? obj.[[GetOwnProperty]](key).
    // b. Let descriptor be ! FromPropertyDescriptor(desc).
    auto descriptorRes = getOwnPropertyDescriptor(runtime, lv.obj, lv.key);
    if (LLVM_UNLIKELY(descriptorRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // c. If descriptor is not undefined,
    //    perform !CreateDataProperty(descriptors, key, descriptor).
    if (!descriptorRes->isUndefined()) {
      lv.descriptor = *descriptorRes;
      auto res = JSObject::defineOwnComputedPrimitive(
          lv.descriptors, runtime, lv.key, dpf, lv.descriptor);
      (void)res;
      assert(
          res != ExecutionStatus::EXCEPTION &&
          "defining own property on a new object cannot fail");
    }
  }
  // 5. Return descriptors.
  return lv.descriptors.getHermesValue();
}

/// Return a list of property names belonging to this object. All
/// properties are converted into strings. The order of
/// properties will remain the same as Object::getOwnPropertyNames.
/// \returns a JSArray containing the names, encoded in HermesValue.
CallResult<HermesValue> getOwnPropertyKeysAsStrings(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    OwnKeysFlags okFlags) {
  auto cr = JSObject::getOwnPropertyKeys(selfHandle, runtime, okFlags);
  if (cr == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<JSArray> array;
    PinnedValue<> prop;
    PinnedValue<> stringValue;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.array = std::move(*cr);
  for (unsigned i = 0, e = lv.array->getEndIndex(); i < e; ++i) {
    lv.prop = lv.array->at(runtime, i).unboxToHV(runtime);
    if (lv.prop->isString() || lv.prop->isSymbol()) {
      // Nothing to do if it's already a string or symbol.
      continue;
    }
    assert(lv.prop->isNumber() && "Property name is either string or number");
    // Otherwise convert it to a string and replace the element.
    auto status = toString_RJS(runtime, lv.prop);
    assert(
        status != ExecutionStatus::EXCEPTION &&
        "toString() on property name cannot fail");
    lv.stringValue = std::move(*status);
    if (LLVM_UNLIKELY(
            JSArray::setElementAt(lv.array, runtime, i, lv.stringValue) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }
  return lv.array.getHermesValue();
}

CallResult<HermesValue> objectGetOwnPropertyNames(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto objRes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<JSObject> objHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.objHandle.castAndSetHermesValue<JSObject>(objRes.getValue());
  auto cr = getOwnPropertyKeysAsStrings(
      lv.objHandle,
      runtime,
      OwnKeysFlags().plusIncludeNonSymbols().plusIncludeNonEnumerable());
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return *cr;
}

CallResult<HermesValue> objectGetOwnPropertySymbols(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto objRes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<JSObject> objHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.objHandle.castAndSetHermesValue<JSObject>(objRes.getValue());
  auto cr = JSObject::getOwnPropertySymbols(lv.objHandle, runtime);
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return cr->getHermesValue();
}

CallResult<bool>
defineProperty(Runtime &runtime, NativeArgs args, PropOpFlags opFlags) {
  // ES9 19.1.2.4 (throwOnError == true) or 26.1.3 (throwOnError == false)
  auto O = args.dyncastArg<JSObject>(0);
  // 1. If Type(O) is not Object, throw a TypeError exception.
  if (!O) {
    return runtime.raiseTypeError(
        "Object.defineProperty() called on non-object");
  }

  // 2. Let key be ? ToPropertyKey(P).
  // Convert the property name to string if it's an object.  This is
  // done explicitly instead of calling defineOwnComputed so that
  // converting the key argument to a primitive happens before
  // toPropertyDescriptor (which can fail, so the order is
  // observable).
  struct : public Locals {
    PinnedValue<> key;
    PinnedValue<> descValueOrAccessor;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  CallResult<PseudoHandle<>> keyRes =
      toPropertyKeyIfObject(runtime, args.getArgHandle(1));
  if (LLVM_UNLIKELY(keyRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  lv.key = std::move(*keyRes);

  // 3. Let desc be ? ToPropertyDescriptor(Attributes).
  DefinePropertyFlags descFlags;
  MutableHandle<> valueAccessor{lv.descValueOrAccessor};
  if (toPropertyDescriptor(
          args.getArgHandle(2), runtime, descFlags, valueAccessor) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  // 4[throwOnError]. Perform ? DefinePropertyOrThrow(O, key, desc).
  // 4[!throwOnError]. Return ? O.[[DefineOwnProperty]](key, desc).
  return JSObject::defineOwnComputedPrimitive(
      O, runtime, lv.key, descFlags, lv.descValueOrAccessor, opFlags);
}

CallResult<HermesValue> objectDefineProperty(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // ES9 19.1.2.4
  CallResult<bool> res =
      defineProperty(runtime, args, PropOpFlags().plusThrowOnError());
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  assert(*res && "defineProperty with throwOnError == true returned false");
  // 5. Return O.
  return args.getArg(0);
}

static CallResult<HermesValue>
objectDefinePropertiesInternal(Runtime &runtime, Handle<> obj, Handle<> props) {
  // Verify this method is called on an object.
  auto *objPtr = dyn_vmcast<JSObject>(obj.get());
  if (!objPtr) {
    return runtime.raiseTypeError(
        "Object.defineProperties() called on non-object");
  }
  struct : public Locals {
    PinnedValue<JSObject> objHandle;
    PinnedValue<JSObject> propsHandle;
    PinnedValue<> propValue;
    PinnedValue<> propName;
    PinnedValue<ArrayStorage> valueOrAccessorStorage;
    PinnedValue<> tempValueOrAccessor;
    PinnedValue<JSArray> propNames;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.objHandle = objPtr;

  // Verify that the properties argument is also an object.
  auto objRes = toObject(runtime, props);
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.propsHandle.castAndSetHermesValue<JSObject>(objRes.getValue());

  // Get the list of identifiers in props.
  auto cr = JSObject::getOwnPropertyKeys(
      lv.propsHandle,
      runtime,
      OwnKeysFlags()
          .plusIncludeSymbols()
          .plusIncludeNonSymbols()
          // setIncludeNonEnumerable for proxies is necessary to get the right
          // traps in the right order.  The non-enumerable props will be
          // filtered out below.
          .setIncludeNonEnumerable(lv.propsHandle->isProxyObject(runtime)));
  if (cr == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.propNames = std::move(*cr);

  // This function may create an unbounded number of GC handles.

  // Iterate through every identifier, get the property descriptor
  // object, and store it in a list, according to Step 5.  What the
  // spec describes is a pair is represented in NewProps, which has
  // the propNameIndex and flags. The valueOrAccessor is stored in
  // ArrayStorage at the same index as this NewProps in the vector.
  struct NewProps {
    unsigned propNameIndex;
    DefinePropertyFlags flags;

    NewProps(unsigned propIdx, DefinePropertyFlags f)
        : propNameIndex(propIdx), flags(f) {}
  };
  llvh::SmallVector<NewProps, 4> newProps;

  // Initialize ArrayStorage to hold valueOrAccessor values
  uint32_t numProps = lv.propNames->getEndIndex();
  auto storageRes = ArrayStorage::create(runtime, numProps, numProps);
  if (LLVM_UNLIKELY(storageRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.valueOrAccessorStorage.castAndSetHermesValue<ArrayStorage>(*storageRes);

  for (unsigned i = 0, e = numProps; i < e; ++i) {
    lv.propName = lv.propNames->at(runtime, i).unboxToHV(runtime);
    ComputedPropertyDescriptor desc;
    CallResult<bool> descRes = JSObject::getOwnComputedDescriptor(
        lv.propsHandle, runtime, lv.propName, desc);
    if (LLVM_UNLIKELY(descRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (LLVM_UNLIKELY(!*descRes || !desc.flags.enumerable)) {
      continue;
    }
    CallResult<PseudoHandle<>> propRes = lv.propsHandle->isProxyObject(runtime)
        ? JSObject::getComputed_RJS(lv.propsHandle, runtime, lv.propName)
        : JSObject::getComputedPropertyValueInternal_RJS(
              lv.propsHandle, runtime, lv.propsHandle, desc);
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    DefinePropertyFlags flags;
    // Initialize tempValueOrAccessor to undefined before each iteration
    lv.tempValueOrAccessor = HermesValue::encodeUndefinedValue();
    MutableHandle<> valueOrAccessor{lv.tempValueOrAccessor};
    lv.propValue = std::move(*propRes);
    if (LLVM_UNLIKELY(
            toPropertyDescriptor(
                lv.propValue, runtime, flags, valueOrAccessor) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // Store the value in ArrayStorage (newProps.size() is the index)
    lv.valueOrAccessorStorage->set(
        newProps.size(), valueOrAccessor.getHermesValue(), runtime.getHeap());
    newProps.emplace_back(i, flags);
  }

  // For each descriptor in the list, add it to the object.
  for (unsigned propIdx = 0; propIdx < newProps.size(); ++propIdx) {
    const auto &newProp = newProps[propIdx];
    lv.propName =
        lv.propNames->at(runtime, newProp.propNameIndex).unboxToHV(runtime);

    // Get the value from ArrayStorage using the same index as newProps
    lv.tempValueOrAccessor = lv.valueOrAccessorStorage->at(propIdx);

    auto result = JSObject::defineOwnComputedPrimitive(
        lv.objHandle,
        runtime,
        lv.propName,
        newProp.flags,
        lv.tempValueOrAccessor,
        PropOpFlags().plusThrowOnError());
    if (result == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return lv.objHandle.getHermesValue();
}

CallResult<HermesValue> objectCreate(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // Verify this method is called with an object or with 'null'.
  auto obj = args.dyncastArg<JSObject>(0);
  if (!obj && !args.getArg(0).isNull()) {
    return runtime.raiseTypeError(
        "Object prototype argument must be an Object or null");
  }

  auto newObj = JSObject::create(runtime, obj).getHermesValue();
  auto arg1 = args.getArgHandle(1);
  if (arg1->isUndefined()) {
    return newObj;
  }
  // Properties argument is present and not undefined.
  struct : public Locals {
    PinnedValue<> newObjHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.newObjHandle = newObj;
  auto cr = objectDefinePropertiesInternal(runtime, lv.newObjHandle, arg1);
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return *cr;
}

CallResult<HermesValue> objectDefineProperties(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto cr = objectDefinePropertiesInternal(
      runtime, args.getArgHandle(0), args.getArgHandle(1));
  if (cr == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return *cr;
}

CallResult<HermesValue> objectSeal(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto objHandle = args.dyncastArg<JSObject>(0);

  if (!objHandle) {
    return args.getArg(0);
  }

  if (LLVM_UNLIKELY(
          JSObject::seal(objHandle, runtime) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return objHandle.getHermesValue();
}

CallResult<HermesValue> objectFreeze(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto objHandle = args.dyncastArg<JSObject>(0);

  if (!objHandle) {
    return args.getArg(0);
  }

  if (LLVM_UNLIKELY(
          JSObject::freeze(objHandle, runtime) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return objHandle.getHermesValue();
}

CallResult<HermesValue> objectPreventExtensions(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  Handle<JSObject> obj = args.dyncastArg<JSObject>(0);

  if (!obj) {
    return args.getArg(0);
  }

  CallResult<bool> statusRes = JSObject::preventExtensions(
      obj, runtime, PropOpFlags().plusThrowOnError());
  if (LLVM_UNLIKELY(statusRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  assert(
      *statusRes &&
      "Object.preventExtensions with ThrowOnError returned false");
  return args.getArg(0);
}

CallResult<HermesValue> objectIs(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return HermesValue::encodeBoolValue(
      isSameValue(args.getArg(0), args.getArg(1)));
}

CallResult<HermesValue> objectIsSealed(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto objHandle = args.dyncastArg<JSObject>(0);

  if (!objHandle) {
    // ES6.0 19.1.2.13: If Type(O) is not Object, return true.
    return HermesValue::encodeBoolValue(true);
  }

  return HermesValue::encodeBoolValue(JSObject::isSealed(objHandle, runtime));
}

CallResult<HermesValue> objectIsFrozen(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto objHandle = args.dyncastArg<JSObject>(0);

  if (!objHandle) {
    // ES6.0 19.1.2.12: If Type(O) is not Object, return true.
    return HermesValue::encodeBoolValue(true);
  }

  return HermesValue::encodeBoolValue(JSObject::isFrozen(objHandle, runtime));
}

CallResult<HermesValue> objectIsExtensible(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  PseudoHandle<JSObject> obj =
      createPseudoHandle(dyn_vmcast<JSObject>(args.getArg(0)));

  if (!obj) {
    // ES6.0 19.1.2.11: If Type(O) is not Object, return false.
    return HermesValue::encodeBoolValue(false);
  }

  CallResult<bool> extRes = JSObject::isExtensible(std::move(obj), runtime);
  if (LLVM_UNLIKELY(extRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return HermesValue::encodeBoolValue(*extRes);
}

/// ES8.0 7.3.21.
/// EnumerableOwnProperties gets the requested properties based on \p kind.
CallResult<HermesValue> enumerableOwnProperties_RJS(
    Runtime &runtime,
    Handle<JSObject> objHandle,
    EnumerableOwnPropertiesKind kind) {
  auto namesRes = getOwnPropertyKeysAsStrings(
      objHandle,
      runtime,
      OwnKeysFlags().plusIncludeNonSymbols().setIncludeNonEnumerable(
          objHandle->isProxyObject(runtime)));
  if (namesRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<JSArray> names;
    PinnedValue<JSArray> properties;
    PinnedValue<StringPrimitive> name;
    PinnedValue<> value;
    PinnedValue<> entry;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  if (kind == EnumerableOwnPropertiesKind::Key &&
      !objHandle->isProxyObject(runtime)) {
    return *namesRes;
  }
  lv.names.castAndSetHermesValue<JSArray>(*namesRes);
  uint32_t len = JSArray::getLength(*lv.names, runtime);

  auto propertiesRes = JSArray::create(runtime, len, len);
  if (LLVM_UNLIKELY(propertiesRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.properties = std::move(*propertiesRes);

  uint32_t targetIdx = 0;

  // Add the requested elements to properties.
  // We must keep track of the targetIdx because elements' enumerability may be
  // modified by a getter at any point in the loop, so `i` will not necessarily
  // correspond to `targetIdx`.
  for (uint32_t i = 0, len = JSArray::getLength(*lv.names, runtime); i < len;
       ++i) {
    lv.name = lv.names->at(runtime, i).getString(runtime);
    // By calling getString, name is guaranteed to be primitive.
    ComputedPropertyDescriptor desc;
    CallResult<bool> descRes = JSObject::getOwnComputedPrimitiveDescriptor(
        objHandle, runtime, lv.name, JSObject::IgnoreProxy::Yes, desc);
    if (LLVM_UNLIKELY(descRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (LLVM_LIKELY(*descRes && desc.flags.enumerable)) {
      // Ensure that the property is still there and that it is enumerable,
      // as descriptors can be modified by a getter at any point.

      // Safe to call the Internal version here because we ignored Proxy above.
      auto valueRes = JSObject::getComputedPropertyValueInternal_RJS(
          objHandle, runtime, objHandle, desc);
      if (LLVM_UNLIKELY(valueRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.value = std::move(*valueRes);
    } else if (!objHandle->isProxyObject(runtime)) {
      continue;
    } else {
      // This is a proxy, so we need to call getOwnProperty() to see
      // if the value exists and is enumerable on the proxy.
      descRes =
          JSProxy::getOwnProperty(objHandle, runtime, lv.name, desc, nullptr);
      if (LLVM_UNLIKELY(descRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (!*descRes || !desc.flags.enumerable) {
        // And skip the property if not.
        continue;
      }
      // And if the caller needs the value, we need to fetch that,
      // too.
      if (kind != EnumerableOwnPropertiesKind::Key) {
        auto valueRes =
            JSProxy::getComputed(objHandle, runtime, lv.name, objHandle);
        if (LLVM_UNLIKELY(valueRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        lv.value = std::move(*valueRes);
      }
    }

    if (kind == EnumerableOwnPropertiesKind::KeyValue) {
      auto entryRes = JSArray::create(runtime, 2, 2);
      if (LLVM_UNLIKELY(entryRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.entry = entryRes->getHermesValue();
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(
                  Handle<JSArray>::vmcast(&lv.entry), runtime, 0, lv.name) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(
                  Handle<JSArray>::vmcast(&lv.entry), runtime, 1, lv.value) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
    } else if (kind == EnumerableOwnPropertiesKind::Value) {
      lv.entry = lv.value.getHermesValue();
    } else {
      assert(
          objHandle->isProxyObject(runtime) &&
          "Key kind did not return early but not proxy");
      lv.entry = lv.names->at(runtime, i).unboxToHV(runtime);
    }

    // The element must exist because we just read it.
    if (LLVM_UNLIKELY(
            JSArray::setElementAt(
                lv.properties, runtime, targetIdx++, lv.entry) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }

  // Set length at the end only, because properties may be shorter than
  // names.size() - some properties may have been made non-enumerable by getters
  // in the loop.
  if (LLVM_UNLIKELY(
          JSArray::setLengthProperty(lv.properties, runtime, targetIdx) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return lv.properties.getHermesValue();
}

namespace {

/// Slots of the "0" and "1" properties of an entry tuple. Tuple element access
/// reads these slots directly, so they must match the slots of a tuple literal.
constexpr SlotIndex kEntryTupleKeySlot = JSObject::numOverlapSlots<JSObject>();
constexpr SlotIndex kEntryTupleValueSlot = kEntryTupleKeySlot + 1;

/// Populate runtime.tupleEntryClass, the hidden class shared by every tuple
/// returned by Object.entries, if it hasn't been created yet. Like tuple
/// literals, entry tuples are typed objects, frozen to untyped code.
/// \param clazz scratch handle used while building the class.
static void ensureTupleEntryClass(
    Runtime &runtime,
    MutableHandle<HiddenClass> clazz) {
  // The class records Object.prototype as its parent, which every entry tuple
  // uses.
  if (LLVM_LIKELY(runtime.tupleEntryClass.get()))
    return;

  clazz = HiddenClass::createForTypedObject(
      runtime, Handle<JSObject>::vmcast(&runtime.objectPrototype), 2);
  for (auto name : {Predefined::zero, Predefined::one}) {
    [[maybe_unused]] SlotIndex slot = HiddenClass::addNewTypedPublicProperty(
        clazz,
        runtime,
        Predefined::getSymbolID(name),
        /* propertiesEnumerable */ true);
    assert(
        slot ==
            (name == Predefined::zero ? kEntryTupleKeySlot
                                      : kEntryTupleValueSlot) &&
        "entry tuple property landed in an unexpected slot");
  }
  runtime.tupleEntryClass = *clazz;
}

/// Create the [key, value] tuple object returned by Object.entries in typed
/// code. It must have the shape IRGen gives tuple literals: own properties
/// "0" and "1", in that order, on a typed object.
/// ensureTupleEntryClass() must have been called first.
/// \param tuple scratch handle keeping the new object alive; also the result.
static void createEntryTuple(
    Runtime &runtime,
    Handle<StringPrimitive> key,
    Handle<> value,
    MutableHandle<JSObject> tuple) {
  tuple = JSObject::create(
              runtime,
              Handle<JSObject>::vmcast(&runtime.objectPrototype),
              Handle<HiddenClass>::vmcast(&runtime.tupleEntryClass))
              .get();
  // Encode the value first, since that may allocate, which would invalidate
  // the pointer in an already encoded key.
  auto valueShv = SmallHermesValue::encodeHermesValue(*value, runtime);
  JSObject::setNamedSlotValueUnsafe(
      *tuple, runtime, kEntryTupleValueSlot, valueShv);
  JSObject::setNamedSlotValueUnsafe(
      *tuple,
      runtime,
      kEntryTupleKeySlot,
      SmallHermesValue::encodeStringValue(*key, runtime));
}

/// Enumerate the dense indexed properties of a FastArray.
CallResult<HermesValue> fastArrayPropsFastArray(
    Runtime &runtime,
    Handle<FastArray> obj,
    Handle<JSObject> prototype,
    EnumerableOwnPropertiesKind kind) {
  struct : public Locals {
    PinnedValue<HiddenClass> tupleClass;
    PinnedValue<JSObject> tuple;
    PinnedValue<FastArray> result;
    PinnedValue<StringPrimitive> key;
    PinnedValue<> value;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  if (kind == EnumerableOwnPropertiesKind::KeyValue)
    ensureTupleEntryClass(runtime, lv.tupleClass);

  const uint32_t length = obj->getLengthAsUint32(runtime);
  auto arrRes = FastArray::create(runtime, prototype, length);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  lv.result.castAndSetHermesValue<FastArray>(*arrRes);

  for (uint32_t index = 0; index < length; ++index) {
    if (kind != EnumerableOwnPropertiesKind::Value) {
      auto keyRes = numberToStringPrimitive(runtime, index);
      if (LLVM_UNLIKELY(keyRes == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      lv.key = std::move(*keyRes);
    }

    switch (kind) {
      case EnumerableOwnPropertiesKind::Key:
        lv.value = lv.key.getHermesValue();
        break;
      case EnumerableOwnPropertiesKind::Value:
        lv.value = obj->unsafeAt(runtime, index).unboxToHV(runtime);
        break;
      case EnumerableOwnPropertiesKind::KeyValue:
        lv.value = obj->unsafeAt(runtime, index).unboxToHV(runtime);
        createEntryTuple(runtime, lv.key, lv.value, lv.tuple);
        lv.value = lv.tuple.getHermesValue();
        break;
    }
    if (LLVM_UNLIKELY(
            FastArray::push(lv.result, runtime, lv.value) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }

  return lv.result.getHermesValue();
}

/// Enumerate an object the slot scan can't handle (proxies, host objects,
/// accessors, indexed storage) through the generic EnumerableOwnProperties, and
/// copy the result into a FastArray. Entries become entry tuples.
CallResult<HermesValue> genericPropsFastArray(
    Runtime &runtime,
    Handle<JSObject> obj,
    Handle<JSObject> prototype,
    EnumerableOwnPropertiesKind kind) {
  struct : public Locals {
    PinnedValue<JSArray> props;
    PinnedValue<JSArray> entry;
    PinnedValue<HiddenClass> tupleClass;
    PinnedValue<JSObject> tuple;
    PinnedValue<FastArray> result;
    PinnedValue<StringPrimitive> key;
    PinnedValue<> value;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  if (kind == EnumerableOwnPropertiesKind::KeyValue)
    ensureTupleEntryClass(runtime, lv.tupleClass);

  auto propsRes = enumerableOwnProperties_RJS(runtime, obj, kind);
  if (LLVM_UNLIKELY(propsRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  lv.props.castAndSetHermesValue<JSArray>(*propsRes);

  const uint32_t length = JSArray::getLength(*lv.props, runtime);
  auto arrRes = FastArray::create(runtime, prototype, length);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  lv.result.castAndSetHermesValue<FastArray>(*arrRes);

  for (uint32_t index = 0; index < length; ++index) {
    lv.value = lv.props->at(runtime, index).unboxToHV(runtime);
    if (kind == EnumerableOwnPropertiesKind::KeyValue) {
      lv.entry.castAndSetHermesValue<JSArray>(lv.value.getHermesValue());
      lv.key.castAndSetHermesValue<StringPrimitive>(
          lv.entry->at(runtime, 0).unboxToHV(runtime));
      lv.value = lv.entry->at(runtime, 1).unboxToHV(runtime);
      createEntryTuple(runtime, lv.key, lv.value, lv.tuple);
      lv.value = lv.tuple.getHermesValue();
    }
    if (LLVM_UNLIKELY(
            FastArray::push(lv.result, runtime, lv.value) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }

  return lv.result.getHermesValue();
}

/// Shared implementation of object{Keys,Values,Entries}FastArray.
CallResult<HermesValue> objectPropsFastArray(
    Runtime &runtime,
    Handle<JSObject> obj,
    Handle<JSObject> prototype,
    EnumerableOwnPropertiesKind kind) {
  if (LLVM_UNLIKELY(vmisa<FastArray>(*obj))) {
    return fastArrayPropsFastArray(
        runtime, Handle<FastArray>::vmcast(obj), prototype, kind);
  }

  // Typed objects never need the generic path, but an `any` argument passed
  // through the implicit checked cast can be any object.
  auto indexedRange = JSObject::getOwnIndexedRange(*obj, runtime);
  if (LLVM_UNLIKELY(
          obj->isProxyObject(runtime) || obj->isHostObject(runtime) ||
          obj->isLazy(runtime) ||
          obj->getClass(runtime)->getMayHaveAccessor() ||
          indexedRange.first != indexedRange.second)) {
    return genericPropsFastArray(runtime, obj, prototype, kind);
  }

  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
    PinnedValue<HiddenClass> tupleClass;
    PinnedValue<JSObject> tuple;
    PinnedValue<FastArray> result;
    PinnedValue<StringPrimitive> key;
    PinnedValue<> value;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  class PropEntry {
   public:
    /// The array index for index-like names, UINT32_MAX otherwise.
    uint32_t index;
    SymbolID name;
    NamedPropertyDescriptor desc;

    PropEntry(uint32_t index, SymbolID name, NamedPropertyDescriptor desc)
        : index(index), name(name), desc(desc) {}
  };
  llvh::SmallVector<PropEntry, 8> props{};
  bool anyArrayIndex = false;

  lv.clazz = obj->getClass(runtime);
  HiddenClass::forEachPropertyWhile(
      lv.clazz,
      runtime,
      [&props, &anyArrayIndex](
          Runtime &runtime, SymbolID id, NamedPropertyDescriptor desc) {
        if (desc.flags.privateName || !desc.flags.enumerable ||
            !isPropertyNamePrimitive(id))
          return true;
        auto arrayIndex = runtime.getIdentifierTable().symbolToArrayIndex(id);
        anyArrayIndex |= arrayIndex.hasValue();
        props.emplace_back(arrayIndex ? *arrayIndex : UINT32_MAX, id, desc);
        return true;
      });

  // Index-like names come first in ascending order; the rest keep insertion
  // order. UINT32_MAX is not a valid array index, so it sorts them last, which
  // means the sort is a no-op unless an index-like name was found.
  if (anyArrayIndex) {
    std::stable_sort(
        props.begin(), props.end(), [](const PropEntry &a, const PropEntry &b) {
          return a.index < b.index;
        });
  }

  if (kind == EnumerableOwnPropertiesKind::KeyValue)
    ensureTupleEntryClass(runtime, lv.tupleClass);

  auto arrRes = FastArray::create(runtime, prototype, props.size());
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  lv.result.castAndSetHermesValue<FastArray>(*arrRes);

  for (const PropEntry &prop : props) {
    lv.key = runtime.getStringPrimFromSymbolID(prop.name);
    switch (kind) {
      case EnumerableOwnPropertiesKind::Key:
        lv.value = lv.key.getHermesValue();
        break;
      case EnumerableOwnPropertiesKind::Value:
        lv.value = JSObject::getNamedSlotValueUnsafe(*obj, runtime, prop.desc)
                       .unboxToHV(runtime);
        break;
      case EnumerableOwnPropertiesKind::KeyValue:
        lv.value = JSObject::getNamedSlotValueUnsafe(*obj, runtime, prop.desc)
                       .unboxToHV(runtime);
        createEntryTuple(runtime, lv.key, lv.value, lv.tuple);
        lv.value = lv.tuple.getHermesValue();
        break;
    }
    if (LLVM_UNLIKELY(
            FastArray::push(lv.result, runtime, lv.value) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }

  return lv.result.getHermesValue();
}

} // namespace

CallResult<HermesValue> objectKeysFastArray(
    Runtime &runtime,
    Handle<JSObject> obj,
    Handle<JSObject> prototype) {
  return objectPropsFastArray(
      runtime, obj, prototype, EnumerableOwnPropertiesKind::Key);
}

CallResult<HermesValue> objectValuesFastArray(
    Runtime &runtime,
    Handle<JSObject> obj,
    Handle<JSObject> prototype) {
  return objectPropsFastArray(
      runtime, obj, prototype, EnumerableOwnPropertiesKind::Value);
}

CallResult<HermesValue> objectEntriesFastArray(
    Runtime &runtime,
    Handle<JSObject> obj,
    Handle<JSObject> prototype) {
  return objectPropsFastArray(
      runtime, obj, prototype, EnumerableOwnPropertiesKind::KeyValue);
}

CallResult<HermesValue> objectKeys(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto objRes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  struct : public Locals {
    PinnedValue<JSObject> objHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.objHandle.castAndSetHermesValue<JSObject>(*objRes);
  return enumerableOwnProperties_RJS(
      runtime, lv.objHandle, EnumerableOwnPropertiesKind::Key);
}

CallResult<HermesValue> objectValues(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto objRes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  struct : public Locals {
    PinnedValue<JSObject> objHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.objHandle.castAndSetHermesValue<JSObject>(*objRes);
  return enumerableOwnProperties_RJS(
      runtime, lv.objHandle, EnumerableOwnPropertiesKind::Value);
}

CallResult<HermesValue> objectEntries(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto objRes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  struct : public Locals {
    PinnedValue<JSObject> objHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.objHandle.castAndSetHermesValue<JSObject>(*objRes);
  return enumerableOwnProperties_RJS(
      runtime, lv.objHandle, EnumerableOwnPropertiesKind::KeyValue);
}

/// ES10 19.1.2.7 Object.fromEntries(iterable)
/// Creates an object from an iterable of [key, value] pairs.
CallResult<HermesValue> objectFromEntries(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. Perform ? RequireObjectCoercible(iterable).
  if (args.getArg(0).isNull() || args.getArg(0).isUndefined()) {
    return runtime.raiseTypeError(
        "fromEntries argument is not coercible to Object");
  }

  // 2. Let obj be ObjectCreate(%ObjectPrototype%).
  struct : public Locals {
    PinnedValue<JSObject> obj;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.obj = JSObject::create(runtime);
  // 3. Assert: obj is an extensible ordinary object with no own properties.

  // 4. Let stepsDefine be the algorithm steps defined in
  //    CreateDataPropertyOnObject Functions.
  // 5. Let adder be CreateBuiltinFunction(stepsDefine, « »).
  // NOTE: We avoid actually creating the NativeFunction here by simply putting
  // the DefineProperty code in the callback.
  // 6. Return ? AddEntriesFromIterable(obj, iterable, adder).
  return addEntriesFromIterable(
      runtime,
      lv.obj,
      args.getArgHandle(0),
      /* method */ llvh::None,
      [&lv, &runtime](Runtime &, Handle<> key, Handle<> value) {
        const DefinePropertyFlags dpf =
            DefinePropertyFlags::getDefaultNewPropertyFlags();
        return JSObject::defineOwnComputed(
            lv.obj, runtime, key, dpf, value, PropOpFlags().plusThrowOnError());
      });
}

CallResult<HermesValue> objectAssign(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  // 1. Let to be ToObject(target).
  auto objRes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    // 2. ReturnIfAbrupt(to).
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<JSObject> toHandle;
    PinnedValue<JSObject> fromHandle;
    PinnedValue<> nextKeyHandle;
    PinnedValue<> propValueHandle;
    PinnedValue<JSArray> keys;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.toHandle.castAndSetHermesValue<JSObject>(objRes.getValue());

  // 3. If only one argument was passed, return to.
  if (LLVM_UNLIKELY(args.getArgCount() == 1)) {
    return lv.toHandle.getHermesValue();
  }

  // 4. Let sources be the List of argument values starting with the second
  // argument.
  // 5. For each element nextSource of sources, in ascending index order,

  // Handle for the current object being copied from.
  MutableHandle<JSObject> fromHandle{lv.fromHandle};
  // Handle for the next key to be processed when copying properties.
  MutableHandle<> nextKeyHandle{lv.nextKeyHandle};
  // Handle for the property value being copied.
  MutableHandle<> propValueHandle{lv.propValueHandle};

  for (uint32_t argIdx = 1; argIdx < args.getArgCount(); argIdx++) {
    auto nextSource = args.getArgHandle(argIdx);
    // 5.a. If nextSource is undefined or null, let keys be an empty List.
    if (nextSource->isNull() || nextSource->isUndefined()) {
      continue;
    }

    // 5.b.i. Let from be ToObject(nextSource).
    if (LLVM_UNLIKELY(
            (objRes = toObject(runtime, nextSource)) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    fromHandle = vmcast<JSObject>(objRes.getValue());

    // 5.b.ii. Let keys be from.[[OwnPropertyKeys]]().
    auto cr = JSObject::getOwnPropertyKeys(
        fromHandle,
        runtime,
        OwnKeysFlags()
            .plusIncludeSymbols()
            .plusIncludeNonSymbols()
            .setIncludeNonEnumerable(fromHandle->isProxyObject(runtime)));
    if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
      // 5.c.ii. ReturnIfAbrupt(keys).
      return ExecutionStatus::EXCEPTION;
    }

    lv.keys = std::move(*cr);
    ComputedPropertyDescriptor desc;
    // 5.c. Repeat for each element nextKey of keys in List order,
    for (uint32_t nextKeyIdx = 0, endIdx = lv.keys->getEndIndex();
         nextKeyIdx < endIdx;
         ++nextKeyIdx) {
      nextKeyHandle = lv.keys->at(runtime, nextKeyIdx).unboxToHV(runtime);

      // 5.c.i. Let desc be from.[[GetOwnProperty]](nextKey).
      auto descCr = JSObject::getOwnComputedDescriptor(
          fromHandle, runtime, nextKeyHandle, desc);
      if (LLVM_UNLIKELY(descCr == ExecutionStatus::EXCEPTION)) {
        // 5.c.ii. ReturnIfAbrupt(desc).
        return ExecutionStatus::EXCEPTION;
      }
      // 5.c.iii. if desc is not undefined and desc.[[Enumerable]] is true, then
      if (LLVM_UNLIKELY(!*descCr) || LLVM_UNLIKELY(!desc.flags.enumerable)) {
        continue;
      }

      // 5.c.iii.1. Let propValue be Get(from, nextKey).

      // getComputed_RJS would work here in all cases.  But, just
      // changing it to make proxy work is is a surprisingly large
      // regression if used always, even with no Proxy objects.  So we
      // check if we can use getComputedPropertyValue_RJS and do so.
      CallResult<PseudoHandle<>> propRes = fromHandle->isProxyObject(runtime)
          ? JSObject::getComputed_RJS(fromHandle, runtime, nextKeyHandle)
          : JSObject::getComputedPropertyValue_RJS(
                fromHandle, runtime, fromHandle, desc, nextKeyHandle);
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        // 5.c.iii.2. ReturnIfAbrupt(propValue).
        return ExecutionStatus::EXCEPTION;
      }
      propValueHandle = std::move(*propRes);

      // 5.c.iii.3. Let status be Set(to, nextKey, propValue, true).
      auto statusCr = JSObject::putComputed_RJS(
          lv.toHandle,
          runtime,
          nextKeyHandle,
          propValueHandle,
          PropOpFlags().plusThrowOnError());
      if (LLVM_UNLIKELY(statusCr == ExecutionStatus::EXCEPTION)) {
        // 5.c.ii.4. ReturnIfAbrupt(status).
        return ExecutionStatus::EXCEPTION;
      }
    }
  }

  // 6 Return to.
  return lv.toHandle.getHermesValue();
}

CallResult<HermesValue> objectSetPrototypeOf(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  Handle<> O = args.getArgHandle(0);
  Handle<> proto = args.getArgHandle(1);
  // 1. Let O be RequireObjectCoercible(O).
  if (O->isNull() || O->isUndefined()) {
    return runtime.raiseTypeError(
        "setPrototypeOf argument is not coercible to Object");
  }

  // 3. If Type(proto) is neither Object nor Null, throw a TypeError exception.
  if (!(proto->isObject() || proto->isNull())) {
    return runtime.raiseTypeError(
        "setPrototypeOf new prototype must be object or null");
  }
  // 4. If Type(O) is not Object, return O.
  if (!vmisa<JSObject>(*O)) {
    return *O;
  }
  // 5. Let status be O.[[SetPrototypeOf]](proto).
  auto status = JSObject::setParent(
      vmcast<JSObject>(*O),
      runtime,
      dyn_vmcast<JSObject>(*proto),
      PropOpFlags().plusThrowOnError());
  // 7. If status is false, throw a TypeError exception.
  // Note that JSObject::setParent throws instead of returning false.
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 8. Return O.
  return *O;
}

/// ES15.0 20.1.2.13 Object.groupBy ( items, callback )
/// https://tc39.es/ecma262/#sec-object.groupby
CallResult<HermesValue> objectGroupBy(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  struct : public Locals {
    PinnedValue<JSObject> obj;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 2. Let obj be OrdinaryObjectCreate(null).
  lv.obj = JSObject::create(runtime, Runtime::makeNullHandle<JSObject>());

  // 1. Let groups be ? GroupBy(items, callbackfn, PROPERTY).
  if (LLVM_UNLIKELY(
          groupByProperty(
              runtime, lv.obj, args.getArgHandle(0), args.getArgHandle(1)) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // NOTE: The below are handled by `groupByProperty` which directly acts on
  // the above JSObject.
  // 3. For each Record { [[Key]], [[Elements]] } g of groups, do
  // a. Let elements be CreateArrayFromList(g.[[Elements]]).
  // b. Perform ! CreateDataPropertyOrThrow(obj, g.[[Key]], elements).

  // 4. Return obj.
  return lv.obj.getHermesValue();
}

//===----------------------------------------------------------------------===//
/// Object.prototype.

CallResult<HermesValue> directObjectPrototypeToString(
    Runtime &runtime,
    Handle<> arg) {
  StringPrimitive *str;

  if (arg->isUndefined()) {
    str = runtime.getPredefinedString(Predefined::squareObjectUndefined);
  } else if (arg->isNull()) {
    str = runtime.getPredefinedString(Predefined::squareObjectNull);
  } else if (arg->getRaw() == runtime.getGlobal().getHermesValue().getRaw()) {
    str = runtime.getPredefinedString(Predefined::squareObjectGlobal);
  } else {
    auto res = toObject(runtime, arg);
    if (res == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }

    struct : public Locals {
      PinnedValue<JSObject> O;
      PinnedValue<StringPrimitive> tag;
      PinnedValue<StringPrimitive> result;
    } lv;
    LocalsRAII lraii(runtime, &lv);
    lv.O.castAndSetHermesValue<JSObject>(res.getValue());
    // 16. Let tag be Get (O, @@toStringTag).
    auto tagRes = JSObject::getNamed_RJS(
        lv.O, runtime, Predefined::getSymbolID(Predefined::SymbolToStringTag));
    if (LLVM_UNLIKELY(tagRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    if ((*tagRes)->isString()) {
      lv.tag = PseudoHandle<StringPrimitive>::vmcast(std::move(*tagRes)).get();
      SafeUInt32 tagLen(lv.tag->getStringLength());
      tagLen.add(9);
      CallResult<StringBuilder> builder = StringBuilder::createStringBuilder(
          runtime, lv.result, tagLen, lv.tag->isASCII());
      if (LLVM_UNLIKELY(builder == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      // 19. Return the String that is the result of concatenating
      // "[object ", tag, and "]".
      builder->appendASCIIRef(ASCIIRef{"[object ", 8});
      builder->appendStringPrim(lv.tag);
      builder->appendCharacter(']');
      return builder->getStringPrimitive().getHermesValue();
    }

    // 18. If Type(tag) is not String, let tag be builtinTag.
    CallResult<bool> isArrayRes = isArray(runtime, lv.O.get());
    if (LLVM_UNLIKELY(isArrayRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (*isArrayRes) {
      // 6. If isArray is true, let builtinTag be "Array".
      str = runtime.getPredefinedString(Predefined::squareObject_JSArray);
    } else if (vmisa<JSString>(lv.O.getHermesValue())) {
      // 7. Else, if O is an exotic String object, let builtinTag be "String".
      str = runtime.getPredefinedString(Predefined::squareObject_JSString);
    } else if (vmisa<Arguments>(lv.O.getHermesValue())) {
      // 8. Else, if O has an [[ParameterMap]] internal slot, let builtinTag be
      // "Arguments".
      str = runtime.getPredefinedString(Predefined::squareObject_Arguments);
    } else if (vmisa<Callable>(lv.O.getHermesValue())) {
      // 9. Else, if O has a [[Call]] internal method, let builtinTag be
      // "Function".
      str = runtime.getPredefinedString(Predefined::squareObject_JSFunction);
    } else if (vmisa<JSError>(lv.O.getHermesValue())) {
      // 10. Else, if O has an [[ErrorData]] internal slot, let builtinTag be
      // "Error".
      str = runtime.getPredefinedString(Predefined::squareObject_JSError);
    } else if (vmisa<JSBoolean>(lv.O.getHermesValue())) {
      // 11. Else, if O has a [[BooleanData]] internal slot, let builtinTag be
      // "Boolean".
      str = runtime.getPredefinedString(Predefined::squareObject_JSBoolean);
    } else if (vmisa<JSNumber>(lv.O.getHermesValue())) {
      // 12. Else, if O has a [[NumberData]] internal slot, let builtinTag be
      // "Number".
      str = runtime.getPredefinedString(Predefined::squareObject_JSNumber);
    } else if (vmisa<JSDate>(lv.O.getHermesValue())) {
      // 13. Else, if O has a [[DateValue]] internal slot, let builtinTag be
      // "Date".
      str = runtime.getPredefinedString(Predefined::squareObject_JSDate);
    } else if (vmisa<JSRegExp>(lv.O.getHermesValue())) {
      // 14. Else, if O has a [[RegExpMatcher]] internal slot, let builtinTag be
      // "RegExp".
      str = runtime.getPredefinedString(Predefined::squareObject_JSRegExp);
    } else {
      str = runtime.getPredefinedString(Predefined::squareObject_JSObject);
    }
  }

  return HermesValue::encodeStringValue(str);
}

CallResult<HermesValue> objectPrototypeToString(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return directObjectPrototypeToString(runtime, args.getThisHandle());
}

CallResult<HermesValue> objectPrototypeToLocaleString(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<JSObject> selfHandle;
    PinnedValue<> propValue;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.selfHandle.castAndSetHermesValue<JSObject>(objRes.getValue());
  auto propRes = JSObject::getNamed_RJS(
      lv.selfHandle, runtime, Predefined::getSymbolID(Predefined::toString));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.propValue = std::move(*propRes);
  if (auto func = Handle<Callable>::dyn_vmcast(Handle<>{lv.propValue})) {
    return Callable::executeCall0(func, runtime, lv.selfHandle)
        .toCallResultHermesValue();
  }
  return runtime.raiseTypeError("toString must be callable");
}

CallResult<HermesValue> objectPrototypeValueOf(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto res = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return res;
}

/// HasOwnProperty(\p obj, \p key) when it can be answered from the class of
/// \p obj alone: \p obj is an object and \p key a uniqued string, for
/// which neither ToObject nor ToPropertyKey has an effect.
/// A named own property is a yes. Its absence is a no only where the slow
/// path stops at it: not for an object with indexed storage, a lazy object,
/// a proxy, or a host object, which claims every property.
/// Does not allocate.
/// \return the answer, or llvh::None if the slow path must give it.
static inline OptValue<bool>
objectHasOwnFast(Runtime &runtime, HermesValue obj, HermesValue key) {
  if (!obj.isObject())
    return llvh::None;
  JSObject *self = vmcast<JSObject>(obj);
  NamedPropertyDescriptor desc;
  OptValue<bool> found =
      JSObject::tryGetOwnNamedDescriptorOfKeyFast(self, runtime, key, desc);
  if (!found)
    return llvh::None;
  if (*found)
    return true;
  const HiddenClass *clazz = self->getClass(runtime);
  if (clazz->hasIndexedStorage() || clazz->isLazyObject() ||
      clazz->isProxyObject() || clazz->isHostObject())
    return llvh::None;
  return false;
}

static CallResult<HermesValue>
objectHasOwnHelper(Runtime &runtime, Handle<JSObject> O, Handle<> P) {
  ComputedPropertyDescriptor desc;
  CallResult<bool> hasProp =
      JSObject::getOwnComputedDescriptor(O, runtime, P, desc);
  if (LLVM_UNLIKELY(hasProp == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (*hasProp) {
    return HermesValue::encodeBoolValue(true);
  }
  // For compatibility with polyfills we want to pretend that all HostObject
  // properties are "own" properties in hasOwnProperty() and in
  // getOwnPropertyDescriptor(). Since there is no way to check for a
  // HostObject property, we must always assume success. In practice the
  // property name would have been obtained from enumerating the properties in
  // JS code that looks something like this:
  //    for(key in hostObj) {
  //      if (Object.hasOwnProperty(hostObj, key))
  //        ...
  //    }
  if (LLVM_UNLIKELY(O->isHostObject(runtime))) {
    return HermesValue::encodeBoolValue(true);
  }
  return HermesValue::encodeBoolValue(false);
}

/// ES11.0 19.1.3.2
CallResult<HermesValue> objectPrototypeHasOwnProperty(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (OptValue<bool> fast =
          objectHasOwnFast(runtime, args.getThisArg(), args.getArg(0)))
    return HermesValue::encodeBoolValue(*fast);
  /// 1. Let P be ? ToPropertyKey(V).
  struct : public Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> P;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  auto PRes = toPropertyKey(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(PRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.P = std::move(*PRes);

  /// 2. Let O be ? ToObject(this value).
  auto ORes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(ORes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  /// 3. Return ? HasOwnProperty(O, P).
  lv.O.castAndSetHermesValue<JSObject>(ORes.getValue());
  return objectHasOwnHelper(runtime, lv.O, lv.P);
}

CallResult<HermesValue> objectHasOwn(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (OptValue<bool> fast =
          objectHasOwnFast(runtime, args.getArg(0), args.getArg(1)))
    return HermesValue::encodeBoolValue(*fast);
  /// 1. Let O be ? ToObject(O).
  auto ORes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(ORes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> P;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.O.castAndSetHermesValue<JSObject>(ORes.getValue());

  /// 2. Let P be ? ToPropertyKey(P).
  auto PRes = toPropertyKey(runtime, args.getArgHandle(1));
  if (LLVM_UNLIKELY(PRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.P = std::move(*PRes);
  /// 3. Return ? HasOwnProperty(O, P).
  return objectHasOwnHelper(runtime, lv.O, lv.P);
}

CallResult<HermesValue> objectPrototypeIsPrototypeOf(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (LLVM_UNLIKELY(!args.getArg(0).isObject())) {
    // If arg[0] is not an object, return false.
    return HermesValue::encodeBoolValue(false);
  }
  auto res = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<JSObject> objHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.objHandle.castAndSetHermesValue<JSObject>(*res);
  PseudoHandle<JSObject> parent =
      createPseudoHandle(vmcast<JSObject>(args.getArg(0)));
  while (true) {
    CallResult<PseudoHandle<JSObject>> protoRes =
        JSObject::getPrototypeOf(std::move(parent), runtime);
    if (LLVM_UNLIKELY(protoRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (!*protoRes) {
      break;
    }
    parent = std::move(*protoRes);
    if (parent.get() == lv.objHandle.get()) {
      return HermesValue::encodeBoolValue(true);
    }
  }
  return HermesValue::encodeBoolValue(false);
}

CallResult<HermesValue> objectPrototypePropertyIsEnumerable(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<JSObject> objHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  auto res = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.objHandle.castAndSetHermesValue<JSObject>(res.getValue());
  ComputedPropertyDescriptor desc;
  auto status = JSObject::getOwnComputedDescriptor(
      lv.objHandle, runtime, args.getArgHandle(0), desc);
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return HermesValue::encodeBoolValue(
      status.getValue() && desc.flags.enumerable);
}

CallResult<HermesValue> objectPrototypeProto_getter(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  CallResult<HermesValue> res = toObject(runtime, args.getThisHandle());
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<JSObject> objHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.objHandle.castAndSetHermesValue<JSObject>(*res);
  return getPrototypeOf(runtime, lv.objHandle);
}

CallResult<HermesValue> objectPrototypeProto_setter(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // thisArg must be coercible to Object.
  if (args.getThisArg().isNull() || args.getThisArg().isUndefined()) {
    return runtime.raiseTypeError("'this' is not coercible to JSObject");
  }
  // But if it isn't an actual object, do nothing.
  if (!args.getThisArg().isObject()) {
    return HermesValue::encodeUndefinedValue();
  }

  HermesValue proto = args.getArg(0);
  JSObject *protoPtr;
  if (proto.isObject())
    protoPtr = vmcast<JSObject>(proto);
  else if (proto.isNull())
    protoPtr = nullptr;
  else
    return HermesValue::encodeUndefinedValue();

  if (LLVM_UNLIKELY(
          JSObject::setParent(
              vmcast<JSObject>(args.getThisArg()),
              runtime,
              protoPtr,
              PropOpFlags().plusThrowOnError()) == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue> objectPrototypeDefineGetter(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<JSObject> O;
    PinnedValue<PropertyAccessor> accessor;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.O.castAndSetHermesValue<JSObject>(objRes.getValue());

  auto getter = args.dyncastArg<Callable>(1);
  if (!getter) {
    return runtime.raiseTypeError("__defineGetter__ getter not callable");
  }

  auto crtRes = PropertyAccessor::create(
      runtime, getter, Runtime::makeNullHandle<Callable>());
  lv.accessor = std::move(crtRes);

  DefinePropertyFlags dpf;
  dpf.setEnumerable = 1;
  dpf.enumerable = 1;
  dpf.setConfigurable = 1;
  dpf.configurable = 1;
  dpf.setGetter = 1;

  auto res = JSObject::defineOwnComputed(
      lv.O,
      runtime,
      args.getArgHandle(0),
      dpf,
      lv.accessor,
      PropOpFlags().plusThrowOnError());
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue> objectPrototypeDefineSetter(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<JSObject> O;
    PinnedValue<PropertyAccessor> accessor;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.O.castAndSetHermesValue<JSObject>(objRes.getValue());

  auto setter = args.dyncastArg<Callable>(1);
  if (!setter) {
    return runtime.raiseTypeError("__defineSetter__ setter not callable");
  }

  auto crtRes = PropertyAccessor::create(
      runtime, Runtime::makeNullHandle<Callable>(), setter);
  lv.accessor = std::move(crtRes);

  DefinePropertyFlags dpf;
  dpf.setEnumerable = 1;
  dpf.enumerable = 1;
  dpf.setConfigurable = 1;
  dpf.configurable = 1;
  dpf.setSetter = 1;

  auto res = JSObject::defineOwnComputed(
      lv.O,
      runtime,
      args.getArgHandle(0),
      dpf,
      lv.accessor,
      PropOpFlags().plusThrowOnError());
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  return HermesValue::encodeUndefinedValue();
}

namespace {

/// Helper function for objectPrototypeLookup{Get,Set}ter.  Returns a
/// pointer to the cell which contains the actual accessors, or
/// nullptr if there aren't any.  This iterates internally because the
/// functions in JSObject are structured in such a way that for
/// complex proxy/target/prototype chains, the accessor is called in a
/// deeply nested place.  To expose it, we need to do the chaining
/// explicitly.
CallResult<PropertyAccessor *> lookupAccessor(
    Runtime &runtime,
    NativeArgs args) {
  CallResult<HermesValue> res = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> valueOrAccessor;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.O.castAndSetHermesValue<JSObject>(*res);
  Handle<> key = args.getArgHandle(0);
  do {
    ComputedPropertyDescriptor desc;
    MutableHandle<> valueOrAccessorHandle{lv.valueOrAccessor};
    CallResult<bool> definedRes = JSObject::getOwnComputedDescriptor(
        lv.O, runtime, key, desc, valueOrAccessorHandle);
    if (LLVM_UNLIKELY(definedRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (*definedRes) {
      if (!desc.flags.accessor) {
        break;
      }
      return vmcast<PropertyAccessor>(lv.valueOrAccessor.getHermesValue());
    }
    CallResult<PseudoHandle<JSObject>> protoRes =
        JSObject::getPrototypeOf(PseudoHandle<JSObject>{lv.O}, runtime);
    if (LLVM_UNLIKELY(protoRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (!*protoRes) {
      lv.O = nullptr;
    } else {
      lv.O.castAndSetHermesValue<JSObject>(protoRes->getHermesValue());
    }
  } while (lv.O.get());
  return nullptr;
}

} // namespace

CallResult<HermesValue> objectPrototypeLookupGetter(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  CallResult<PropertyAccessor *> accessorRes = lookupAccessor(runtime, args);
  if (LLVM_UNLIKELY(accessorRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return (*accessorRes && (*accessorRes)->getter)
      ? HermesValue::encodeObjectValue(
            (*accessorRes)->getter.getNonNull(runtime))
      : HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue> objectPrototypeLookupSetter(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  CallResult<PropertyAccessor *> accessorRes = lookupAccessor(runtime, args);
  if (LLVM_UNLIKELY(accessorRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return (*accessorRes && (*accessorRes)->setter)
      ? HermesValue::encodeObjectValue(
            (*accessorRes)->setter.getNonNull(runtime))
      : HermesValue::encodeUndefinedValue();
}

} // namespace vm
} // namespace hermes

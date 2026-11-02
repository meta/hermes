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

#include "hermes/VM/HermesValueTraits.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/PrimitiveBox.h"
#include "hermes/VM/PropertyAccessor.h"
#include "hermes/VM/StringBuilder.h"

namespace hermes {
namespace vm {

//===----------------------------------------------------------------------===//
/// Object.

Handle<JSObject> createObjectConstructor(Runtime &runtime) {
  auto objectPrototype = Handle<JSObject>::vmcast(&runtime.objectPrototype);

  auto cons = defineSystemConstructor<JSObject>(
      runtime,
      Predefined::getSymbolID(Predefined::Object),
      objectConstructor,
      Handle<JSObject>::vmcast(&runtime.objectPrototype),
      1,
      CellKind::JSObjectKind);
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
      cons,
      Predefined::getSymbolID(Predefined::getPrototypeOf),
      ctx,
      objectGetPrototypeOf,
      1);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::getOwnPropertyDescriptor),
      ctx,
      objectGetOwnPropertyDescriptor,
      2);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::getOwnPropertyDescriptors),
      ctx,
      objectGetOwnPropertyDescriptors,
      1);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::getOwnPropertyNames),
      ctx,
      objectGetOwnPropertyNames,
      1);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::getOwnPropertySymbols),
      ctx,
      objectGetOwnPropertySymbols,
      1);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::hasOwn),
      ctx,
      objectHasOwn,
      2);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::seal),
      ctx,
      objectSeal,
      1);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::freeze),
      ctx,
      objectFreeze,
      1);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::fromEntries),
      ctx,
      objectFromEntries,
      1);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::preventExtensions),
      ctx,
      objectPreventExtensions,
      1);

  defineMethod(
      runtime, cons, Predefined::getSymbolID(Predefined::is), ctx, objectIs, 2);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::isSealed),
      ctx,
      objectIsSealed,
      1);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::isFrozen),
      ctx,
      objectIsFrozen,
      1);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::isExtensible),
      ctx,
      objectIsExtensible,
      1);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::keys),
      ctx,
      objectKeys,
      1);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::values),
      ctx,
      objectValues,
      1);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::entries),
      ctx,
      objectEntries,
      1);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::create),
      ctx,
      objectCreate,
      2);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::defineProperty),
      ctx,
      objectDefineProperty,
      3);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::defineProperties),
      ctx,
      objectDefineProperties,
      2);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::assign),
      ctx,
      objectAssign,
      2);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::setPrototypeOf),
      ctx,
      objectSetPrototypeOf,
      2);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::groupBy),
      ctx,
      objectGroupBy,
      2);

  return cons;
}

/// ES2024 20.1.1.1 Object() invoked as a function and as a
/// constructor.
CallResult<HermesValue>
objectConstructor(void *, Runtime &runtime, NativeArgs args) {
  auto &newTarget = args.getNewTarget();
  // 1. If NewTarget is neither undefined nor the active function object, then
  if (!newTarget.isUndefined() &&
      newTarget.getRaw() != runtime.objectConstructor.getRaw()) {
    // a. Return ? OrdinaryCreateFromConstructor(NewTarget,
    // "%Object.prototype%").
    // This is a construct call, which means the object we want to create was
    // actually already created for us in the caller and passed as `this`, so we
    // can just use that.
    assert(
        args.getThisArg().isObject() &&
        "'this' must be an object in a constructor call");
    return args.getThisArg();
  }

  auto value = args.getArgHandle(0);
  // 2. If value is either undefined or null, return
  // OrdinaryObjectCreate(%Object.prototype%).
  if (value->isUndefined() || value->isNull()) {
    // If this is a construct call directly on Object, then
    // OrdinaryObjectCreate(%Object.prototype%) is the value of `this`.
    if (newTarget.getRaw() == runtime.objectConstructor.getRaw()) {
      return args.getThisArg();
    }
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

CallResult<HermesValue>
objectGetPrototypeOf(void *, Runtime &runtime, NativeArgs args) {
  CallResult<HermesValue> res = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return getPrototypeOf(runtime, runtime.makeHandle(vmcast<JSObject>(*res)));
}

CallResult<HermesValue> getOwnPropertyDescriptor(
    Runtime &runtime,
    Handle<JSObject> object,
    Handle<> key) {
  ComputedPropertyDescriptor desc;
  MutableHandle<> valueOrAccessor{runtime};
  MutableHandle<SymbolID> tmpPropNameStorage{runtime};
  {
    auto result = JSObject::getOwnComputedDescriptor(
        object, runtime, key, tmpPropNameStorage, desc, valueOrAccessor);
    if (result == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    if (!*result) {
      if (LLVM_LIKELY(!object->isHostObject()))
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
    valueOrAccessor = std::move(*propRes);
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

  return objectFromPropertyDescriptor(runtime, dpFlags, valueOrAccessor);
}

CallResult<HermesValue>
objectGetOwnPropertyDescriptor(void *, Runtime &runtime, NativeArgs args) {
  auto objRes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<JSObject> O = runtime.makeHandle<JSObject>(objRes.getValue());

  return getOwnPropertyDescriptor(runtime, O, args.getArgHandle(1));
}

/// ES10.0 19.1.2.9
CallResult<HermesValue>
objectGetOwnPropertyDescriptors(void *, Runtime &runtime, NativeArgs args) {
  GCScope gcScope{runtime};

  // 1. Let obj be ? ToObject(O).
  auto objRes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<JSObject> obj = runtime.makeHandle<JSObject>(objRes.getValue());

  // 2. Let ownKeys be ? obj.[[OwnPropertyKeys]]().
  auto ownKeysRes = JSObject::getOwnPropertyKeys(
      obj,
      runtime,
      OwnKeysFlags()
          .plusIncludeNonSymbols()
          .plusIncludeSymbols()
          .plusIncludeNonEnumerable());
  if (LLVM_UNLIKELY(ownKeysRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<JSArray> ownKeys = *ownKeysRes;
  uint32_t len = JSArray::getLength(*ownKeys, runtime);

  // 3. Let descriptors be ! ObjectCreate(%ObjectPrototype%).
  auto descriptors = runtime.makeHandle<JSObject>(JSObject::create(runtime));

  MutableHandle<> key{runtime};
  MutableHandle<> descriptor{runtime};

  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();

  auto marker = gcScope.createMarker();
  // 4. For each element key of ownKeys in List order, do
  for (uint32_t i = 0; i < len; ++i) {
    gcScope.flushToMarker(marker);
    key = ownKeys->at(runtime, i).unboxToHV(runtime);
    // a. Let desc be ? obj.[[GetOwnProperty]](key).
    // b. Let descriptor be ! FromPropertyDescriptor(desc).
    auto descriptorRes = getOwnPropertyDescriptor(runtime, obj, key);
    if (LLVM_UNLIKELY(descriptorRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // c. If descriptor is not undefined,
    //    perform !CreateDataProperty(descriptors, key, descriptor).
    if (!descriptorRes->isUndefined()) {
      descriptor = *descriptorRes;
      auto res = JSObject::defineOwnComputedPrimitive(
          descriptors, runtime, key, dpf, descriptor);
      (void)res;
      assert(
          res != ExecutionStatus::EXCEPTION &&
          "defining own property on a new object cannot fail");
    }
  }
  // 5. Return descriptors.
  return descriptors.getHermesValue();
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
  auto array = *cr;
  MutableHandle<> prop(runtime);
  GCScope gcScope(runtime);
  auto marker = gcScope.createMarker();
  for (unsigned i = 0, e = array->getEndIndex(); i < e; ++i) {
    gcScope.flushToMarker(marker);
    prop = array->at(runtime, i).unboxToHV(runtime);
    if (prop->isString() || prop->isSymbol()) {
      // Nothing to do if it's already a string or symbol.
      continue;
    }
    assert(prop->isNumber() && "Property name is either string or number");
    // Otherwise convert it to a string and replace the element.
    auto status = toString_RJS(runtime, prop);
    assert(
        status != ExecutionStatus::EXCEPTION &&
        "toString() on property name cannot fail");
    JSArray::setElementAt(
        array, runtime, i, runtime.makeHandle(std::move(*status)));
  }
  return array.getHermesValue();
}

CallResult<HermesValue>
objectGetOwnPropertyNames(void *, Runtime &runtime, NativeArgs args) {
  auto objRes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto objHandle = runtime.makeHandle<JSObject>(objRes.getValue());
  auto cr = getOwnPropertyKeysAsStrings(
      objHandle,
      runtime,
      OwnKeysFlags().plusIncludeNonSymbols().plusIncludeNonEnumerable());
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return *cr;
}

CallResult<HermesValue>
objectGetOwnPropertySymbols(void *, Runtime &runtime, NativeArgs args) {
  auto objRes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto objHandle = runtime.makeHandle<JSObject>(objRes.getValue());
  auto cr = JSObject::getOwnPropertySymbols(objHandle, runtime);
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
  CallResult<Handle<>> keyRes =
      toPropertyKeyIfObject(runtime, args.getArgHandle(1));
  if (LLVM_UNLIKELY(keyRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;

  // 3. Let desc be ? ToPropertyDescriptor(Attributes).
  DefinePropertyFlags descFlags;
  MutableHandle<> descValueOrAccessor{runtime};
  if (toPropertyDescriptor(
          args.getArgHandle(2), runtime, descFlags, descValueOrAccessor) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  // 4[throwOnError]. Perform ? DefinePropertyOrThrow(O, key, desc).
  // 4[!throwOnError]. Return ? O.[[DefineOwnProperty]](key, desc).
  return JSObject::defineOwnComputedPrimitive(
      O, runtime, *keyRes, descFlags, descValueOrAccessor, opFlags);
}

CallResult<HermesValue>
objectDefineProperty(void *, Runtime &runtime, NativeArgs args) {
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
  auto objHandle = runtime.makeHandle(objPtr);

  // Verify that the properties argument is also an object.
  auto objRes = toObject(runtime, props);
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto propsHandle = runtime.makeHandle<JSObject>(objRes.getValue());

  // Get the list of identifiers in props.
  auto cr = JSObject::getOwnPropertyKeys(
      propsHandle,
      runtime,
      OwnKeysFlags()
          .plusIncludeSymbols()
          .plusIncludeNonSymbols()
          // setIncludeNonEnumerable for proxies is necessary to get the right
          // traps in the right order.  The non-enumerable props will be
          // filtered out below.
          .setIncludeNonEnumerable(propsHandle->isProxyObject()));
  if (cr == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto propNames = *cr;

  // This function may create an unbounded number of GC handles.
  GCScope scope{runtime, "objectDefinePropertiesInternal", UINT_MAX};

  // Iterate through every identifier, get the property descriptor
  // object, and store it in a list, according to Step 5.  What the
  // spec describes is a pair is represented in NewProps, which has
  // three arguments because the spec descriptor is represented here
  // by flags and a handle.
  struct NewProps {
    unsigned propNameIndex;
    DefinePropertyFlags flags;
    MutableHandle<> valueOrAccessor;

    NewProps(unsigned i, DefinePropertyFlags f, MutableHandle<> voa)
        : propNameIndex(i), flags(f), valueOrAccessor(std::move(voa)) {}
  };
  llvh::SmallVector<NewProps, 4> newProps;

  // We store each property name here. This is hoisted out of the loop
  // to avoid allocating a handle per property.
  MutableHandle<> propName{runtime};
  MutableHandle<SymbolID> tmpPropNameStorage{runtime};

  for (unsigned i = 0, e = propNames->getEndIndex(); i < e; ++i) {
    propName = propNames->at(runtime, i).unboxToHV(runtime);
    ComputedPropertyDescriptor desc;
    CallResult<bool> descRes = JSObject::getOwnComputedDescriptor(
        propsHandle, runtime, propName, tmpPropNameStorage, desc);
    if (LLVM_UNLIKELY(descRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (LLVM_UNLIKELY(!*descRes || !desc.flags.enumerable)) {
      continue;
    }
    CallResult<PseudoHandle<>> propRes = propsHandle->isProxyObject()
        ? JSObject::getComputed_RJS(propsHandle, runtime, propName)
        : JSObject::getComputedPropertyValueInternal_RJS(
              propsHandle, runtime, propsHandle, desc);
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    DefinePropertyFlags flags;
    MutableHandle<> valueOrAccessor{runtime};
    if (LLVM_UNLIKELY(
            toPropertyDescriptor(
                runtime.makeHandle(std::move(*propRes)),
                runtime,
                flags,
                valueOrAccessor) == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    newProps.emplace_back(i, flags, std::move(valueOrAccessor));
  }

  // For each descriptor in the list, add it to the object.
  for (const auto &newProp : newProps) {
    propName = propNames->at(runtime, newProp.propNameIndex).unboxToHV(runtime);
    auto result = JSObject::defineOwnComputedPrimitive(
        objHandle,
        runtime,
        propName,
        newProp.flags,
        newProp.valueOrAccessor,
        PropOpFlags().plusThrowOnError());
    if (result == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return objHandle.getHermesValue();
}

CallResult<HermesValue>
objectCreate(void *, Runtime &runtime, NativeArgs args) {
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
  auto cr =
      objectDefinePropertiesInternal(runtime, runtime.makeHandle(newObj), arg1);
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return *cr;
}

CallResult<HermesValue>
objectDefineProperties(void *, Runtime &runtime, NativeArgs args) {
  auto cr = objectDefinePropertiesInternal(
      runtime, args.getArgHandle(0), args.getArgHandle(1));
  if (cr == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return *cr;
}

CallResult<HermesValue> objectSeal(void *, Runtime &runtime, NativeArgs args) {
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

CallResult<HermesValue>
objectFreeze(void *, Runtime &runtime, NativeArgs args) {
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

CallResult<HermesValue>
objectPreventExtensions(void *, Runtime &runtime, NativeArgs args) {
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

CallResult<HermesValue> objectIs(void *, Runtime &runtime, NativeArgs args) {
  return HermesValue::encodeBoolValue(
      isSameValue(args.getArg(0), args.getArg(1)));
}

CallResult<HermesValue>
objectIsSealed(void *, Runtime &runtime, NativeArgs args) {
  auto objHandle = args.dyncastArg<JSObject>(0);

  if (!objHandle) {
    // ES6.0 19.1.2.13: If Type(O) is not Object, return true.
    return HermesValue::encodeBoolValue(true);
  }

  return HermesValue::encodeBoolValue(JSObject::isSealed(objHandle, runtime));
}

CallResult<HermesValue>
objectIsFrozen(void *, Runtime &runtime, NativeArgs args) {
  auto objHandle = args.dyncastArg<JSObject>(0);

  if (!objHandle) {
    // ES6.0 19.1.2.12: If Type(O) is not Object, return true.
    return HermesValue::encodeBoolValue(true);
  }

  return HermesValue::encodeBoolValue(JSObject::isFrozen(objHandle, runtime));
}

CallResult<HermesValue>
objectIsExtensible(void *, Runtime &runtime, NativeArgs args) {
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
  GCScope gcScope{runtime};

  auto namesRes = getOwnPropertyKeysAsStrings(
      objHandle,
      runtime,
      OwnKeysFlags().plusIncludeNonSymbols().setIncludeNonEnumerable(
          objHandle->isProxyObject()));
  if (namesRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  if (kind == EnumerableOwnPropertiesKind::Key && !objHandle->isProxyObject()) {
    return *namesRes;
  }
  auto names = runtime.makeHandle<JSArray>(*namesRes);
  uint32_t len = JSArray::getLength(*names, runtime);

  auto propertiesRes = JSArray::create(runtime, len, len);
  if (LLVM_UNLIKELY(propertiesRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto properties = *propertiesRes;

  MutableHandle<StringPrimitive> name{runtime};
  MutableHandle<> value{runtime};
  MutableHandle<> entry{runtime};
  MutableHandle<SymbolID> tmpPropNameStorage{runtime};

  uint32_t targetIdx = 0;

  // Add the requested elements to properties.
  // We must keep track of the targetIdx because elements' enumerability may be
  // modified by a getter at any point in the loop, so `i` will not necessarily
  // correspond to `targetIdx`.
  auto marker = gcScope.createMarker();
  for (uint32_t i = 0, len = JSArray::getLength(*names, runtime); i < len;
       ++i) {
    gcScope.flushToMarker(marker);

    name = names->at(runtime, i).getString(runtime);
    // By calling getString, name is guaranteed to be primitive.
    ComputedPropertyDescriptor desc;
    CallResult<bool> descRes = JSObject::getOwnComputedPrimitiveDescriptor(
        objHandle,
        runtime,
        name,
        JSObject::IgnoreProxy::Yes,
        tmpPropNameStorage,
        desc);
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
      value = std::move(*valueRes);
    } else if (!objHandle->isProxyObject()) {
      continue;
    } else {
      // This is a proxy, so we need to call getOwnProperty() to see
      // if the value exists and is enumerable on the proxy.
      descRes =
          JSProxy::getOwnProperty(objHandle, runtime, name, desc, nullptr);
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
            JSProxy::getComputed(objHandle, runtime, name, objHandle);
        if (LLVM_UNLIKELY(valueRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        value = std::move(*valueRes);
      }
    }

    if (kind == EnumerableOwnPropertiesKind::KeyValue) {
      auto entryRes = JSArray::create(runtime, 2, 2);
      if (LLVM_UNLIKELY(entryRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      entry = entryRes->getHermesValue();
      JSArray::setElementAt(Handle<JSArray>::vmcast(entry), runtime, 0, name);
      JSArray::setElementAt(Handle<JSArray>::vmcast(entry), runtime, 1, value);
    } else if (kind == EnumerableOwnPropertiesKind::Value) {
      entry = value.getHermesValue();
    } else {
      assert(
          objHandle->isProxyObject() &&
          "Key kind did not return early but not proxy");
      entry = names->at(runtime, i).unboxToHV(runtime);
    }

    // The element must exist because we just read it.
    JSArray::setElementAt(properties, runtime, targetIdx++, entry);
  }

  // Set length at the end only, because properties may be shorter than
  // names.size() - some properties may have been made non-enumerable by getters
  // in the loop.
  if (LLVM_UNLIKELY(
          JSArray::setLengthProperty(properties, runtime, targetIdx) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return properties.getHermesValue();
}

CallResult<HermesValue> objectKeys(void *, Runtime &runtime, NativeArgs args) {
  auto objRes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return enumerableOwnProperties_RJS(
      runtime,
      runtime.makeHandle<JSObject>(*objRes),
      EnumerableOwnPropertiesKind::Key);
}

CallResult<HermesValue>
objectValues(void *, Runtime &runtime, NativeArgs args) {
  auto objRes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return enumerableOwnProperties_RJS(
      runtime,
      runtime.makeHandle<JSObject>(*objRes),
      EnumerableOwnPropertiesKind::Value);
}

CallResult<HermesValue>
objectEntries(void *, Runtime &runtime, NativeArgs args) {
  auto objRes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return enumerableOwnProperties_RJS(
      runtime,
      runtime.makeHandle<JSObject>(*objRes),
      EnumerableOwnPropertiesKind::KeyValue);
}

/// ES10 19.1.2.7 Object.fromEntries(iterable)
/// Creates an object from an iterable of [key, value] pairs.
CallResult<HermesValue>
objectFromEntries(void *, Runtime &runtime, NativeArgs args) {
  // 1. Perform ? RequireObjectCoercible(iterable).
  if (args.getArg(0).isNull() || args.getArg(0).isUndefined()) {
    return runtime.raiseTypeError(
        "fromEntries argument is not coercible to Object");
  }

  GCScopeMarkerRAII marker{runtime};

  // 2. Let obj be ObjectCreate(%ObjectPrototype%).
  Handle<JSObject> obj = runtime.makeHandle(JSObject::create(runtime));
  // 3. Assert: obj is an extensible ordinary object with no own properties.

  // 4. Let stepsDefine be the algorithm steps defined in
  //    CreateDataPropertyOnObject Functions.
  // 5. Let adder be CreateBuiltinFunction(stepsDefine, « »).
  // NOTE: We avoid actually creating the NativeFunction here by simply putting
  // the DefineProperty code in the callback.
  // 6. Return ? AddEntriesFromIterable(obj, iterable, adder).
  return addEntriesFromIterable(
      runtime,
      obj,
      args.getArgHandle(0),
      [obj, &runtime](Runtime &, Handle<> key, Handle<> value) {
        const DefinePropertyFlags dpf =
            DefinePropertyFlags::getDefaultNewPropertyFlags();
        return JSObject::defineOwnComputed(
            obj, runtime, key, dpf, value, PropOpFlags().plusThrowOnError());
      });
}

CallResult<HermesValue>
objectAssign(void *, Runtime &runtime, NativeArgs args) {
  vm::GCScope gcScope(runtime);

  // 1. Let to be ToObject(target).
  auto objRes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    // 2. ReturnIfAbrupt(to).
    return ExecutionStatus::EXCEPTION;
  }
  auto toHandle = runtime.makeHandle<JSObject>(objRes.getValue());

  // 3. If only one argument was passed, return to.
  if (LLVM_UNLIKELY(args.getArgCount() == 1)) {
    return toHandle.getHermesValue();
  }

  // 4. Let sources be the List of argument values starting with the second
  // argument.
  // 5. For each element nextSource of sources, in ascending index order,

  // Handle for the current object being copied from.
  MutableHandle<JSObject> fromHandle{runtime};
  // Handle for the next key to be processed when copying properties.
  MutableHandle<> nextKeyHandle{runtime};
  // Handle for the property value being copied.
  MutableHandle<> propValueHandle{runtime};
  // Handle for the property value being copied.
  MutableHandle<SymbolID> tmpPropNameStorage{runtime};

  for (uint32_t argIdx = 1; argIdx < args.getArgCount(); argIdx++) {
    GCScopeMarkerRAII markerOuter(gcScope);
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
            .setIncludeNonEnumerable(fromHandle->isProxyObject()));
    if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
      // 5.c.ii. ReturnIfAbrupt(keys).
      return ExecutionStatus::EXCEPTION;
    }

    auto keys = *cr;
    ComputedPropertyDescriptor desc;
    // 5.c. Repeat for each element nextKey of keys in List order,
    for (uint32_t nextKeyIdx = 0, endIdx = keys->getEndIndex();
         nextKeyIdx < endIdx;
         ++nextKeyIdx) {
      GCScopeMarkerRAII markerInner(gcScope);

      nextKeyHandle = keys->at(runtime, nextKeyIdx).unboxToHV(runtime);

      // 5.c.i. Let desc be from.[[GetOwnProperty]](nextKey).
      auto descCr = JSObject::getOwnComputedDescriptor(
          fromHandle, runtime, nextKeyHandle, tmpPropNameStorage, desc);
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
      CallResult<PseudoHandle<>> propRes = fromHandle->isProxyObject()
          ? JSObject::getComputed_RJS(fromHandle, runtime, nextKeyHandle)
          : JSObject::getComputedPropertyValue_RJS(
                fromHandle,
                runtime,
                fromHandle,
                tmpPropNameStorage,
                desc,
                nextKeyHandle);
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        // 5.c.iii.2. ReturnIfAbrupt(propValue).
        return ExecutionStatus::EXCEPTION;
      }
      propValueHandle = std::move(*propRes);

      // 5.c.iii.3. Let status be Set(to, nextKey, propValue, true).
      auto statusCr = JSObject::putComputed_RJS(
          toHandle,
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
  return toHandle.getHermesValue();
}

CallResult<HermesValue>
objectSetPrototypeOf(void *, Runtime &runtime, NativeArgs args) {
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

CallResult<HermesValue>
objectGroupBy(void *, Runtime &runtime, NativeArgs args) {
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

  Handle<JSObject> O = runtime.makeHandle(JSObject::create(runtime));
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

    // get the group array
    auto groupArrayRes = JSObject::getComputed_RJS(O, runtime, *propertyKeyRes);
    if (LLVM_UNLIKELY(groupArrayRes == ExecutionStatus::EXCEPTION)) {
      return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
    }

    // new group key, no array in object yet so create it
    if (groupArrayRes.getValue()->isUndefined() || groupArrayRes.getValue()->isNull()) {
      auto targetGroupArrayRes = JSArray::create(runtime, 0, 0);
      if (LLVM_UNLIKELY(targetGroupArrayRes == ExecutionStatus::EXCEPTION)) {
        return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
      }

      targetGroupArray = std::move(*targetGroupArrayRes);

      // group already created, get the array
      if (LLVM_UNLIKELY(JSObject::defineOwnComputed(O, runtime, groupKey, DefinePropertyFlags().getDefaultNewPropertyFlags(), targetGroupArray, PropOpFlags().plusThrowOnError()) == ExecutionStatus::EXCEPTION)) {
        return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
      }
    } else {
      objectArrayHandle = runtime.makeHandle(std::move(groupArrayRes.getValue()));
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

    auto O = runtime.makeHandle<JSObject>(res.getValue());
    // 16. Let tag be Get (O, @@toStringTag).
    auto tagRes = JSObject::getNamed_RJS(
        O, runtime, Predefined::getSymbolID(Predefined::SymbolToStringTag));
    if (LLVM_UNLIKELY(tagRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    if ((*tagRes)->isString()) {
      auto tag = runtime.makeHandle(
          PseudoHandle<StringPrimitive>::vmcast(std::move(*tagRes)));
      SafeUInt32 tagLen(tag->getStringLength());
      tagLen.add(9);
      CallResult<StringBuilder> builder =
          StringBuilder::createStringBuilder(runtime, tagLen);
      if (LLVM_UNLIKELY(builder == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      // 19. Return the String that is the result of concatenating
      // "[object ", tag, and "]".
      builder->appendASCIIRef(ASCIIRef{"[object ", 8});
      builder->appendStringPrim(tag);
      builder->appendCharacter(']');
      return builder->getStringPrimitive().getHermesValue();
    }

    // 18. If Type(tag) is not String, let tag be builtinTag.
    CallResult<bool> isArrayRes = isArray(runtime, *O);
    if (LLVM_UNLIKELY(isArrayRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (*isArrayRes) {
      // 6. If isArray is true, let builtinTag be "Array".
      str = runtime.getPredefinedString(Predefined::squareObject_JSArray);
    } else if (vmisa<JSString>(O.getHermesValue())) {
      // 7. Else, if O is an exotic String object, let builtinTag be "String".
      str = runtime.getPredefinedString(Predefined::squareObject_JSString);
    } else if (vmisa<Arguments>(O.getHermesValue())) {
      // 8. Else, if O has an [[ParameterMap]] internal slot, let builtinTag be
      // "Arguments".
      str = runtime.getPredefinedString(Predefined::squareObject_Arguments);
    } else if (vmisa<Callable>(O.getHermesValue())) {
      // 9. Else, if O has a [[Call]] internal method, let builtinTag be
      // "Function".
      str = runtime.getPredefinedString(Predefined::squareObject_JSFunction);
    } else if (vmisa<JSError>(O.getHermesValue())) {
      // 10. Else, if O has an [[ErrorData]] internal slot, let builtinTag be
      // "Error".
      str = runtime.getPredefinedString(Predefined::squareObject_JSError);
    } else if (vmisa<JSBoolean>(O.getHermesValue())) {
      // 11. Else, if O has a [[BooleanData]] internal slot, let builtinTag be
      // "Boolean".
      str = runtime.getPredefinedString(Predefined::squareObject_JSBoolean);
    } else if (vmisa<JSNumber>(O.getHermesValue())) {
      // 12. Else, if O has a [[NumberData]] internal slot, let builtinTag be
      // "Number".
      str = runtime.getPredefinedString(Predefined::squareObject_JSNumber);
    } else if (vmisa<JSDate>(O.getHermesValue())) {
      // 13. Else, if O has a [[DateValue]] internal slot, let builtinTag be
      // "Date".
      str = runtime.getPredefinedString(Predefined::squareObject_JSDate);
    } else if (vmisa<JSRegExp>(O.getHermesValue())) {
      // 14. Else, if O has a [[RegExpMatcher]] internal slot, let builtinTag be
      // "RegExp".
      str = runtime.getPredefinedString(Predefined::squareObject_JSRegExp);
    } else {
      str = runtime.getPredefinedString(Predefined::squareObject_JSObject);
    }
  }

  return HermesValue::encodeStringValue(str);
}

CallResult<HermesValue>
objectPrototypeToString(void *, Runtime &runtime, NativeArgs args) {
  return directObjectPrototypeToString(runtime, args.getThisHandle());
}

CallResult<HermesValue>
objectPrototypeToLocaleString(void *, Runtime &runtime, NativeArgs args) {
  GCScope gcScope(runtime);
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto selfHandle = runtime.makeHandle<JSObject>(objRes.getValue());
  auto propRes = JSObject::getNamed_RJS(
      selfHandle, runtime, Predefined::getSymbolID(Predefined::toString));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (auto func = Handle<Callable>::dyn_vmcast(
          runtime.makeHandle(std::move(*propRes)))) {
    return Callable::executeCall0(func, runtime, selfHandle)
        .toCallResultHermesValue();
  }
  return runtime.raiseTypeError("toString must be callable");
}

CallResult<HermesValue>
objectPrototypeValueOf(void *, Runtime &runtime, NativeArgs args) {
  auto res = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return res;
}

static CallResult<HermesValue>
objectHasOwnHelper(Runtime &runtime, Handle<JSObject> O, Handle<> P) {
  ComputedPropertyDescriptor desc;
  MutableHandle<SymbolID> tmpPropNameStorage{runtime};
  CallResult<bool> hasProp = JSObject::getOwnComputedDescriptor(
      O, runtime, P, tmpPropNameStorage, desc);
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
  if (LLVM_UNLIKELY(O->isHostObject())) {
    return HermesValue::encodeBoolValue(true);
  }
  return HermesValue::encodeBoolValue(false);
}

/// ES11.0 19.1.3.2
CallResult<HermesValue>
objectPrototypeHasOwnProperty(void *, Runtime &runtime, NativeArgs args) {
  /// 1. Let P be ? ToPropertyKey(V).
  auto PRes = toPropertyKey(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(PRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  /// 2. Let O be ? ToObject(this value).
  auto ORes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(ORes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  /// 3. Return ? HasOwnProperty(O, P).
  Handle<JSObject> O = runtime.makeHandle<JSObject>(ORes.getValue());
  return objectHasOwnHelper(runtime, O, *PRes);
}

CallResult<HermesValue>
objectHasOwn(void *, Runtime &runtime, NativeArgs args) {
  /// 1. Let O be ? ToObject(O).
  auto ORes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(ORes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<JSObject> O = runtime.makeHandle<JSObject>(ORes.getValue());

  /// 2. Let P be ? ToPropertyKey(P).
  auto PRes = toPropertyKey(runtime, args.getArgHandle(1));
  if (LLVM_UNLIKELY(PRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  /// 3. Return ? HasOwnProperty(O, P).
  return objectHasOwnHelper(runtime, O, *PRes);
}

CallResult<HermesValue>
objectPrototypeIsPrototypeOf(void *, Runtime &runtime, NativeArgs args) {
  if (LLVM_UNLIKELY(!args.getArg(0).isObject())) {
    // If arg[0] is not an object, return false.
    return HermesValue::encodeBoolValue(false);
  }
  auto res = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<JSObject> objHandle = runtime.makeHandle<JSObject>(*res);
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
    if (parent.get() == objHandle.get()) {
      return HermesValue::encodeBoolValue(true);
    }
  }
  return HermesValue::encodeBoolValue(false);
}

CallResult<HermesValue>
objectPrototypePropertyIsEnumerable(void *, Runtime &runtime, NativeArgs args) {
  MutableHandle<SymbolID> tmpPropNameStorage{runtime};
  auto res = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  ComputedPropertyDescriptor desc;
  auto status = JSObject::getOwnComputedDescriptor(
      runtime.makeHandle<JSObject>(res.getValue()),
      runtime,
      args.getArgHandle(0),
      tmpPropNameStorage,
      desc);
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return HermesValue::encodeBoolValue(
      status.getValue() && desc.flags.enumerable);
}

CallResult<HermesValue>
objectPrototypeProto_getter(void *, Runtime &runtime, NativeArgs args) {
  CallResult<HermesValue> res = toObject(runtime, args.getThisHandle());
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return getPrototypeOf(runtime, runtime.makeHandle(vmcast<JSObject>(*res)));
}

CallResult<HermesValue>
objectPrototypeProto_setter(void *, Runtime &runtime, NativeArgs args) {
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

CallResult<HermesValue>
objectPrototypeDefineGetter(void *, Runtime &runtime, NativeArgs args) {
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto O = runtime.makeHandle<JSObject>(objRes.getValue());

  auto getter = args.dyncastArg<Callable>(1);
  if (!getter) {
    return runtime.raiseTypeError("__defineGetter__ getter not callable");
  }

  HermesValue crtRes = PropertyAccessor::create(
      runtime, getter, Runtime::makeNullHandle<Callable>());
  auto accessor = runtime.makeHandle<PropertyAccessor>(crtRes);

  DefinePropertyFlags dpf;
  dpf.setEnumerable = 1;
  dpf.enumerable = 1;
  dpf.setConfigurable = 1;
  dpf.configurable = 1;
  dpf.setGetter = 1;

  auto res = JSObject::defineOwnComputed(
      O,
      runtime,
      args.getArgHandle(0),
      dpf,
      accessor,
      PropOpFlags().plusThrowOnError());
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue>
objectPrototypeDefineSetter(void *, Runtime &runtime, NativeArgs args) {
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto O = runtime.makeHandle<JSObject>(objRes.getValue());

  auto setter = args.dyncastArg<Callable>(1);
  if (!setter) {
    return runtime.raiseTypeError("__defineSetter__ setter not callable");
  }

  HermesValue crtRes = PropertyAccessor::create(
      runtime, Runtime::makeNullHandle<Callable>(), setter);
  auto accessor = runtime.makeHandle<PropertyAccessor>(crtRes);

  DefinePropertyFlags dpf;
  dpf.setEnumerable = 1;
  dpf.enumerable = 1;
  dpf.setConfigurable = 1;
  dpf.configurable = 1;
  dpf.setSetter = 1;

  auto res = JSObject::defineOwnComputed(
      O,
      runtime,
      args.getArgHandle(0),
      dpf,
      accessor,
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
  MutableHandle<JSObject> O = runtime.makeMutableHandle(vmcast<JSObject>(*res));
  Handle<> key = args.getArgHandle(0);
  MutableHandle<> valueOrAccessor{runtime};
  MutableHandle<SymbolID> tmpPropNameStorage{runtime};
  do {
    ComputedPropertyDescriptor desc;
    CallResult<bool> definedRes = JSObject::getOwnComputedDescriptor(
        O, runtime, key, tmpPropNameStorage, desc, valueOrAccessor);
    if (LLVM_UNLIKELY(definedRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (*definedRes) {
      if (!desc.flags.accessor) {
        break;
      }
      return vmcast<PropertyAccessor>(valueOrAccessor.get());
    }
    CallResult<PseudoHandle<JSObject>> protoRes =
        JSObject::getPrototypeOf(O, runtime);
    if (LLVM_UNLIKELY(protoRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    O = protoRes->get();
  } while (O);
  return nullptr;
}

} // namespace

CallResult<HermesValue>
objectPrototypeLookupGetter(void *, Runtime &runtime, NativeArgs args) {
  CallResult<PropertyAccessor *> accessorRes = lookupAccessor(runtime, args);
  if (LLVM_UNLIKELY(accessorRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return (*accessorRes && (*accessorRes)->getter)
      ? HermesValue::encodeObjectValue(
            (*accessorRes)->getter.getNonNull(runtime))
      : HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue>
objectPrototypeLookupSetter(void *, Runtime &runtime, NativeArgs args) {
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

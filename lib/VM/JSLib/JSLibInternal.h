/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_JSLIB_JSLIBINTERNAL_H
#define HERMES_VM_JSLIB_JSLIBINTERNAL_H

#include "hermes/Support/ScopeChain.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/JSDate.h"
#include "hermes/VM/JSError.h"
#include "hermes/VM/JSMapImpl.h"
#include "hermes/VM/JSNativeFunctions.h"
#include "hermes/VM/JSProxy.h"
#include "hermes/VM/JSRegExp.h"
#include "hermes/VM/JSWeakRef.h"

namespace hermes {
namespace vm {

/// This function declares a new system constructor (the likes of 'Object' and
/// 'Array') with a specified object to be used as the 'prototype' property.
/// This function must only be called during initialization and the assumption
/// is that all object operations will succeed. Thus, it should never fail.
/// - First, it creates a new NativeFunction object for the constructor, with
///   Function.prototype as the internal prototype.
/// - Initializes the native function's 'prototype' property with the supplied
///   prototype object.
/// - Then, it sets the supplied prototype object's 'constructor' property to
///   the created NativeFunction.
/// - Lastly, it initializes the requested global property with the
///   NativeFunction.
///
/// \param name the name of the global property to initialize (e.g. 'Object')
/// \param nativeFunctionPtr the native function implementing the constructor.
/// \param prototypeObjectHandle the object instance to set in the 'prototype'
///   property of the constructor.
/// \param paramCount the number of declared constructor parameters
/// \param constructorProtoObjectHandle the prototype to set for the
///   constructor function
/// \param constructorOut a mutable handle to store the created constructor
/// \return the created constructor function, stored in \p constructorOut.
Handle<NativeConstructor> defineSystemConstructor(
    Runtime &runtime,
    SymbolID name,
    NativeFunctionPtr nativeFunctionPtr,
    Handle<JSObject> prototypeObjectHandle,
    Handle<JSObject> constructorProtoObjectHandle,
    unsigned paramCount,
    MutableHandle<NativeConstructor> constructorOut);

Handle<NativeConstructor> defineSystemConstructor(
    Runtime &runtime,
    SymbolID name,
    NativeFunctionPtr nativeFunctionPtr,
    Handle<JSObject> prototypeObjectHandle,
    unsigned paramCount,
    MutableHandle<NativeConstructor> constructorOut);

/// Define a method in an object instance.
/// Currently, it's only used to define global %HermesInternal object in
/// createHermesInternalObject(), with different flags, i.e. writable = 0 and
/// configurable = 0. This function must only be called during initialization
/// and the assumption is that all object operations will succeed.
/// Thus, it should never fail.
/// \param objectHandle the instance where the method is defined.
/// \param propertyName the key in objectHandle to insert the method at.
/// \param methodName the value of the function.name property.
/// \param context the context to pass to the native function.
/// \param nativeFunctionPtr the native function implementing the method.
/// \param paramCount the number of declared method parameters
/// \param dpf the flags to set on the newly defined property.
/// \return the new NativeFunction.
NativeFunction *defineMethod(
    Runtime &runtime,
    Handle<JSObject> objectHandle,
    SymbolID propertyName,
    SymbolID methodName,
    void *context,
    NativeFunctionPtr nativeFunctionPtr,
    unsigned paramCount,
    DefinePropertyFlags dpf);

/// Define a method in an object instance.
/// \param objectHandle the instance where the method is defined.
/// \param name the name of the method.
/// \param context the context to pass to the native function.
/// \param nativeFunctionPtr the native function implementing the method.
/// \param paramCount the number of declared method parameters
NativeFunction *defineMethod(
    Runtime &runtime,
    Handle<JSObject> objectHandle,
    SymbolID name,
    void *context,
    NativeFunctionPtr nativeFunctionPtr,
    unsigned paramCount);

/// Define an accessor in an object instance.
/// \param objectHandle the instance where the accessor is defined.
/// \param propertyName the key in the object at which to define the accessor.
/// \param methodName the name of the function.
/// \param context the context to pass to the native functions.
/// \param getterFunc the native function implementing the getter.
/// \param setterFunc the native function implementing the setter.
void defineAccessor(
    Runtime &runtime,
    Handle<JSObject> objectHandle,
    SymbolID propertyName,
    SymbolID methodName,
    void *context,
    NativeFunctionPtr getterFunc,
    NativeFunctionPtr setterFunc,
    bool enumerable,
    bool configurable);

/// Define an accessor in an object instance.
/// \param objectHandle the instance where the accessor is defined.
/// \param name the name of the accessor.
/// \param context the context to pass to the native functions.
/// \param getterFunc the native function implementing the getter.
/// \param setterFunc the native function implementing the setter.
inline void defineAccessor(
    Runtime &runtime,
    Handle<JSObject> objectHandle,
    SymbolID name,
    void *context,
    NativeFunctionPtr getterFunc,
    NativeFunctionPtr setterFunc,
    bool enumerable,
    bool configurable) {
  defineAccessor(
      runtime,
      objectHandle,
      name,
      name,
      context,
      getterFunc,
      setterFunc,
      enumerable,
      configurable);
}

/// Define a property in an object instance.
/// The property is writable, configurable, but not enumerable.
/// \param objectHandle the instance where the property is defined.
/// \param name the name of the property.
/// \param value the value to set to the property.
void defineProperty(
    Runtime &runtime,
    Handle<JSObject> objectHandle,
    SymbolID name,
    Handle<> value);

/// Define a property in an object instance.
/// \param objectHandle the instance where the property is defined.
/// \param name the name of the property.
/// \param value the value to set to the property.
/// \param dpf the flags to set on the newly defined property.
void defineProperty(
    Runtime &runtime,
    Handle<JSObject> objectHandle,
    SymbolID name,
    Handle<> value,
    DefinePropertyFlags dpf);

/// Create and initialize the global Object constructor. Populate the methods
/// of Object and Object.prototype.
/// \return the global Object constructor.
HermesValue createObjectConstructor(Runtime &runtime);

/// Built-in Object.prototype.toString.
CallResult<HermesValue> directObjectPrototypeToString(
    Runtime &runtime,
    Handle<> arg);

/// Create and initialize the global Error constructor, as well as all
/// the native error constructors. Populate the instance and prototype methods.
#define ALL_ERROR_TYPE(name) \
  HermesValue create##name##Constructor(Runtime &runtime);
#include "hermes/VM/NativeErrorTypes.def"

/// Populate the internal CallSite.prototype.
void populateCallSitePrototype(Runtime &runtime);

/// Create and initialize the global String constructor. Populate the methods
/// of String and String.prototype.
/// \return the global String constructor.
HermesValue createStringConstructor(Runtime &runtime);

/// Create and initialize the global BigInt constructor. Populate the methods
/// of BigInt and BigInt.prototype.
/// \return the global BigInt constructor.
void createBigIntConstructor(Runtime &runtime);

/// Create and initialize the global Function constructor. Populate the methods
/// of Function and Function.prototype.
/// \return the global Function constructor.
HermesValue createFunctionConstructor(Runtime &runtime);

/// Create and initialize the global Number constructor. Populate the methods
/// of Number and Number.prototype.
/// \return the global Number constructor.
HermesValue createNumberConstructor(Runtime &runtime);

/// Create and initialize the global Boolean constructor. Populate the methods
/// of Boolean and Boolean.prototype.
/// \return the global Boolean constructor.
HermesValue createBooleanConstructor(Runtime &runtime);

/// Create and initialize the global Date constructor. Populate the methods
/// of Date and Date.prototype.
/// \return the global Date constructor.
HermesValue createDateConstructor(Runtime &runtime);

/// Create and initialize the global Math object, populating its value
/// and function properties.
HermesValue createMathObject(Runtime &runtime);

/// Create and initialize the global Proxy constructor, populating its methods.
/// \return the global Proxy constructor.
HermesValue createProxyConstructor(Runtime &runtime);

// Forward declaration.
class JSLibFlags;

/// Create and initialize the global %HermesInternal object, populating its
/// value and function properties.
HermesValue createHermesInternalObject(
    Runtime &runtime,
    const JSLibFlags &jsLibFlags);

#ifdef HERMES_ENABLE_DEBUGGER

/// Create and initialize the global %DebuggerInternal object, populating its
/// value and function properties.
Handle<JSObject> createDebuggerInternalObject(Runtime &runtime);

#endif // HERMES_ENABLE_DEBUGGER

/// Create and initialize the global JSON object, populating its value
/// and function properties.
void createJSONObject(Runtime &runtime, MutableHandle<JSObject> result);

/// Create and initialize the global Reflect object, populating its value
/// and function properties.
void createReflectObject(Runtime &runtime, MutableHandle<JSObject> result);

/// Create and initialize the global RegExp constructor. Populate the methods
/// of RegExp and RegExp.prototype.
/// \return the global RegExp constructor.
HermesValue createRegExpConstructor(Runtime &runtime);

/// ES6.0 21.2.3.2.3 Runtime Semantics: RegExpCreate ( P, F )
/// Creates a new RegExp with provided pattern \p P, and flags \p F.
ExecutionStatus regExpCreate(
    Runtime &runtime,
    Handle<> P,
    Handle<> F,
    MutableHandle<JSRegExp> regExpOut);

/// ES6.0 21.2.5.2.1
/// Implemented in RegExp.cpp
CallResult<HermesValue>
regExpExec(Runtime &runtime, Handle<JSObject> R, Handle<StringPrimitive> S);

/// Runs the RegExp.prototype.exec() function (ES5.1 15.10.6.2)
/// with a this value of \p regexp, with the argument \p S.
/// \return a new array as the result, null pointer if there were no matches.
ExecutionStatus directRegExpExec(
    Handle<JSRegExp> regexp,
    Runtime &runtime,
    Handle<StringPrimitive> S,
    MutableHandle<JSArray> resultOut);

/// ES6.0 21.1.3.14.1
/// Implemented in RegExp.cpp
/// Transforms a replacement string by substituting $ replacement strings.
/// \p captures can be a null pointer.
CallResult<HermesValue> getSubstitution(
    Runtime &runtime,
    Handle<StringPrimitive> matched,
    Handle<StringPrimitive> str,
    uint32_t position,
    Handle<ArrayStorageSmall> captures,
    Handle<JSObject> namedCaptures,
    Handle<StringPrimitive> replacement);

/// Main logic for String.prototype.split and RegExp.prototype[Symbol.split].
/// Returns an array of splitted strings.
CallResult<HermesValue> splitInternal(
    Runtime &runtime,
    Handle<> string,
    Handle<> limit,
    Handle<> separator);

/// Set the lastIndex property of \p regexp to \p shv.
inline ExecutionStatus
setLastIndex(Handle<JSObject> regexp, Runtime &runtime, SmallHermesValue shv) {
  return runtime.putNamedThrowOnError(
      regexp, PropCacheID::RegExpLastIndex, shv);
}

/// Set the lastIndex property of \p regexp to \p value.
inline ExecutionStatus
setLastIndex(Handle<JSObject> regexp, Runtime &runtime, double value) {
  auto shv = SmallHermesValue::encodeNumberValue(value, runtime);
  return setLastIndex(regexp, runtime, shv);
}

/// ES6.0 21.2.5.2.3
/// If \p unicode is set and the character at \p index in \S is the start of a
/// surrogate pair, \return index + 2. Otherwise \return index + 1.
/// Note that this function does not allocate.
uint64_t
advanceStringIndex(const StringPrimitive *S, uint64_t index, bool unicode);

/// Create and initialize the global Array constructor. Populate the methods
/// of Array and Array.prototype.
/// \return the global Array constructor.
HermesValue createArrayConstructor(Runtime &runtime);

/// Copy the elements of \p src, an array of length \p len, into \p dest, and
/// set the length of \p dest to \p len. Holes become undefined, which is what
/// reading them from \p src returns, since arrayFastPathCheck ensured that the
/// prototype chain has no indexed properties in bounds.
/// \pre \p src passes arrayFastPathCheck with length \p len.
/// \pre \p dest is a new empty array from JSArray::create.
/// \pre \p dest has capacity for at least \p len elements, since the copy
///   does not grow its storage.
void arrayFastPathCopy(
    Runtime &runtime,
    Handle<JSArray> dest,
    Handle<JSArray> src,
    uint32_t len);

HermesValue createArrayBufferConstructor(Runtime &runtime);

HermesValue createDataViewConstructor(Runtime &runtime);

HermesValue createTypedArrayBaseConstructor(Runtime &runtime);

#define TYPED_ARRAY(name, type) \
  HermesValue create##name##ArrayConstructor(Runtime &runtime);
#include "hermes/VM/TypedArrays.def"
#undef TYPED_ARRAY

/// Add Uint8Array-specific prototype methods (toBase64, toHex, setFromBase64,
/// setFromHex) and static methods (fromBase64, fromHex) per the TC39
/// "Uint8Array to/from Base64 and Hex" proposal:
/// https://tc39.es/proposal-arraybuffer-base64/spec/
void populateUint8ArrayBuiltins(Runtime &runtime);

/// Create and initialize the global Set constructor. Populate the methods
/// of Set.prototype.
HermesValue createSetConstructor(Runtime &runtime);

/// Create SetIterator.prototype and populate methods.
HermesValue createSetIteratorPrototype(Runtime &runtime);

/// Create and initialize the global Map constructor. Populate the methods
/// of Map.prototype.
HermesValue createMapConstructor(Runtime &runtime);

/// Create MapIterator.prototype and populate methods.
HermesValue createMapIteratorPrototype(Runtime &runtime);

/// Create the WeakMap constructor and populate methods.
HermesValue createWeakMapConstructor(Runtime &runtime);

/// Create the WeakSet constructor and populate methods.
HermesValue createWeakSetConstructor(Runtime &runtime);

/// Create the WeakRef constructor and populate methods.
HermesValue createWeakRefConstructor(Runtime &runtime);

/// Create the FinalizationRegistry constructor and populate methods.
HermesValue createFinalizationRegistryConstructor(Runtime &runtime);

/// Create the Symbol constructor and populate methods.
void createSymbolConstructor(Runtime &runtime);

/// Create the GeneratorFunction constructor and populate methods.
HermesValue createGeneratorFunctionConstructor(Runtime &runtime);

/// Create the AsyncFunction constructor and populate methods.
HermesValue createAsyncFunctionConstructor(Runtime &runtime);

/// Create the IteratorPrototype.
void populateIteratorPrototype(Runtime &runtime);

/// Create the ArrayIterator prototype.
void populateArrayIteratorPrototype(Runtime &runtime);

/// Create the StringIterator prototype.
void populateStringIteratorPrototype(Runtime &runtime);

/// Create the RegExpStringIterator prototype.
void populateRegExpStringIteratorPrototype(Runtime &runtime);

/// Create the %GeneratorPrototype%.
void populateGeneratorPrototype(Runtime &runtime);

/// ES19.2.1.1.1. CreateDynamicFunction ( constructor, newTarget, kind, args )
enum class DynamicFunctionKind { Normal, Generator, Async };

/// Create a new function given arguments and a body.
/// \param isGeneratorFunction when true, make a generator with "function*".
CallResult<HermesValue> createDynamicFunction(
    Runtime &runtime,
    NativeArgs args,
    DynamicFunctionKind kind);

/// A direct passthrough to call eval() on \p str.
CallResult<HermesValue> directEval(
    Runtime &runtime,
    Handle<StringPrimitive> str,
    bool strictCaller,
    const CodeBlock *codeBlock,
    bool singleFunction = false);

/// ES10 23.1.1.2 AddEntriesFromIterable
/// Calls a callback with each pair of [key, value] from an iterable.
/// \param target the object to which to add the entries
/// \param iterable iterable which contains pairs of [key, value].
///     Must not be undefined or null.
/// \param method (optional) the Symbol.iterator method of the iterable,
///   if the caller has already retrieved it.
/// \param adder the callback for actually adding properties, with signature:
///     ExecutionStatus adder(Runtime &runtime, Handle<> key, Handle<> value);
template <typename AdderCB>
CallResult<HermesValue> addEntriesFromIterable(
    Runtime &runtime,
    Handle<JSObject> target,
    Handle<> iterable,
    llvh::Optional<Handle<Callable>> method,
    AdderCB adder) {
  GCScope gcScope{runtime};
  // 2. Assert: iterable is present, and is neither undefined nor null.
  assert(
      !iterable->isUndefined() && !iterable->isNull() &&
      "iterable cannot be undefined or null");
  // 3. Let iteratorRecord be ? GetIterator(iterable).
  CallResult<CheckedIteratorRecord> iterRes =
      getCheckedIterator(runtime, iterable, method);
  if (LLVM_UNLIKELY(iterRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto iteratorRecord = *iterRes;

  struct : public Locals {
    PinnedValue<JSObject> nextItem;
    PinnedValue<> key;
    PinnedValue<> value;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  auto marker = gcScope.createMarker();

  // 4. Repeat,
  for (;; gcScope.flushToMarker(marker)) {
    // a. Let next be ? IteratorStep(iteratorRecord).
    auto nextRes = iteratorStep(runtime, iteratorRecord);
    if (LLVM_UNLIKELY(nextRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (!*nextRes) {
      // b. If next is false, return target.
      return target.getHermesValue();
    }
    // c. Let nextItem be ? IteratorValue(next).
    lv.nextItem.template castAndSetHermesValue<JSObject>(
        nextRes->getHermesValue());
    auto nextItemRes = JSObject::getNamed_RJS(
        lv.nextItem, runtime, Predefined::getSymbolID(Predefined::value));
    if (LLVM_UNLIKELY(nextItemRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (!vmisa<JSObject>(nextItemRes->get())) {
      // d. If Type(nextItem) is not Object, then
      // i.     Let error be ThrowCompletion(a newly created TypeError object).
      // ii.     Return ? IteratorClose(iteratorRecord, error).
      (void)runtime.raiseTypeError("Iterator value must be an object");
      return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
    }
    lv.nextItem.template castAndSetHermesValue<JSObject>(
        nextItemRes->getHermesValue());

    // e. Let k be Get(nextItem, "0").
    auto keyRes = getIndexed_RJS(runtime, lv.nextItem, 0);
    if (LLVM_UNLIKELY(keyRes == ExecutionStatus::EXCEPTION)) {
      // f. If k is an abrupt completion,
      //    return ? IteratorClose(iteratorRecord, k).
      return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
    }
    lv.key = std::move(*keyRes);

    // g. Let v be Get(nextItem, "1").
    auto valueRes = getIndexed_RJS(runtime, lv.nextItem, 1);
    if (LLVM_UNLIKELY(valueRes == ExecutionStatus::EXCEPTION)) {
      // h. If v is an abrupt completion,
      //    return ? IteratorClose(iteratorRecord, v).
      return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
    }
    lv.value = std::move(*valueRes);

    // i. Let status be Call(adder, target, « k.[[Value]], v.[[Value]] »).
    if (LLVM_UNLIKELY(
            adder(runtime, lv.key, lv.value) == ExecutionStatus::EXCEPTION)) {
      // j. If status is an abrupt completion,
      //    return ? IteratorClose(iteratorRecord, status).
      return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
    }
  }

  llvm_unreachable(
      "loop must terminate with 'return' when iteration is complete");
}

} // namespace vm

#ifdef HERMES_ENABLE_INTL
namespace intl {

// TODO T65916424: Consider how we can move this somewhere more modular.
vm::HermesValue createIntlObject(vm::Runtime &runtime);

} // namespace intl
#endif

} // namespace hermes

#endif // HERMES_VM_JSLIB_JSLIBINTERNAL_H

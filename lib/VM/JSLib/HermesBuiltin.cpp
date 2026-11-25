/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSLibInternal.h"

#include "hermes/FrontEndDefs/Builtins.h"
#include "hermes/FrontEndDefs/Typeof.h"
#include "hermes/Support/Base64vlq.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/FastArray.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/JSLib.h"
#include "hermes/VM/JSRegExp.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/PrimitiveBox.h"
#include "hermes/VM/StackFrame-inline.h"
#include "hermes/VM/StringBuilder.h"
#include "hermes/VM/StringView.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace hermes {
namespace vm {

/// Set the parent of an object failing silently on any error.
CallResult<HermesValue> silentObjectSetPrototypeOf(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  JSObject *O = dyn_vmcast<JSObject>(args.getArg(0));
  if (!O)
    return HermesValue::encodeUndefinedValue();

  JSObject *parent;
  HermesValue V = args.getArg(1);
  if (V.isNull())
    parent = nullptr;
  else if (V.isObject())
    parent = vmcast<JSObject>(V);
  else
    return HermesValue::encodeUndefinedValue();

  (void)JSObject::setParent(O, runtime, parent);

  // Ignore exceptions.
  runtime.clearThrownValue();

  return HermesValue::encodeUndefinedValue();
}

/// ES6.0 12.2.9.3 Runtime Semantics: GetTemplateObject ( templateLiteral )
/// Given a template literal, return a template object that looks like this:
/// [cookedString0, cookedString1, ..., raw: [rawString0, rawString1]].
/// This object is frozen, as well as the 'raw' object nested inside.
/// We only pass the parts from the template literal that are needed to
/// construct this object. That is, the raw strings and cooked strings.
/// Arguments: \p templateObjID is the unique id associated with the template
/// object. \p dup is a boolean, when it is true, cooked strings are the same as
/// raw strings. Then raw strings are passed. Finally cooked strings are
/// optionally passed if \p dup is true.
CallResult<HermesValue> hermesBuiltinGetTemplateObject(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (LLVM_UNLIKELY(args.getArgCount() < 3)) {
    return runtime.raiseTypeError("At least three arguments expected");
  }
  if (LLVM_UNLIKELY(!args.getArg(0).isNumber())) {
    return runtime.raiseTypeError("First argument should be a number");
  }
  if (LLVM_UNLIKELY(!args.getArg(1).isBool())) {
    return runtime.raiseTypeError("Second argument should be a bool");
  }

  struct : public Locals {
    PinnedValue<JSArray> rawObj;
    PinnedValue<JSArray> templateObj;
    PinnedValue<> idx;
    PinnedValue<> rawValue;
    PinnedValue<> cookedValue;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Try finding the template object in the template object cache.
  uint32_t templateObjID = args.getArg(0).getNumberAs<uint32_t>();

  // Retrieve the code block of the caller to get the cache.
  auto frames = runtime.getStackFrames();
  auto it = frames.begin();
  if (LLVM_UNLIKELY(++it == frames.end()))
    return runtime.raiseTypeError("Cannot be called directly");
  auto callerCB = it->getCalleeCodeBlock();
  if (LLVM_UNLIKELY(!callerCB)) {
    return runtime.raiseTypeError("Cannot be called from native code");
  }
  RuntimeModule *runtimeModule = callerCB->getRuntimeModule();
  JSObject *cachedTemplateObj =
      runtimeModule->findCachedTemplateObject(templateObjID);
  if (cachedTemplateObj) {
    return HermesValue::encodeObjectValue(cachedTemplateObj);
  }

  bool dup = args.getArg(1).getBool();
  if (LLVM_UNLIKELY(!dup && args.getArgCount() % 2 == 1)) {
    return runtime.raiseTypeError(
        "There must be the same number of raw and cooked strings.");
  }
  uint32_t count = dup ? args.getArgCount() - 2 : args.getArgCount() / 2 - 1;

  // Create template object and raw object.
  auto arrRes = JSArray::create(runtime, count, 0);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.rawObj = std::move(*arrRes);
  auto arrRes2 = JSArray::create(runtime, count, 0);
  if (LLVM_UNLIKELY(arrRes2 == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.templateObj = std::move(*arrRes2);

  // Set cooked and raw strings as elements in template object and raw object,
  // respectively.
  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.configurable = 0;
  uint32_t cookedBegin = dup ? 2 : 2 + count;
  for (uint32_t i = 0; i < count; ++i) {
    lv.idx = HermesValue::encodeTrustedNumberValue(i);

    lv.cookedValue = args.getArg(cookedBegin + i);
    auto putRes = JSObject::defineOwnComputedPrimitive(
        lv.templateObj, runtime, lv.idx, dpf, lv.cookedValue);
    assert(
        putRes != ExecutionStatus::EXCEPTION && *putRes &&
        "Failed to set cooked value to template object.");

    lv.rawValue = args.getArg(2 + i);
    putRes = JSObject::defineOwnComputedPrimitive(
        lv.rawObj, runtime, lv.idx, dpf, lv.rawValue);
    assert(
        putRes != ExecutionStatus::EXCEPTION && *putRes &&
        "Failed to set raw value to raw object.");
  }

  if (LLVM_UNLIKELY(
          setTemplateObjectProps(runtime, lv.templateObj, lv.rawObj) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;

  // Cache the template object.
  runtimeModule->cacheTemplateObject(templateObjID, lv.templateObj);

  return lv.templateObj.getHermesValue();
}

/// If the first argument is not an object, throw a type error with the second
/// argument as a message.
///
/// \code
///   HermesBuiltin.ensureObject = function(value, errorMessage) {...}
/// \endcode
CallResult<HermesValue> hermesBuiltinEnsureObject(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (LLVM_LIKELY(args.getArg(0).isObject()))
    return HermesValue::encodeUndefinedValue();

  return runtime.raiseTypeError(args.getArgHandle(1));
}

/// Perform the GetMethod() abstract operation.
///
/// \code
///   HermesBuiltin.getMethod = function(object, property) {...}
/// \endcode
CallResult<HermesValue> hermesBuiltinGetMethod(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return getMethod(runtime, args.getArgHandle(0), args.getArgHandle(1))
      .toCallResultHermesValue();
}

/// Throw a type error with the argument as a message.
///
/// \code
///   HermesBuiltin.throwTypeError = function(errorMessage) {...}
/// \endcode
CallResult<HermesValue> hermesBuiltinThrowTypeError(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return runtime.raiseTypeError(args.getArgHandle(0));
}

/// Check that \p value matches the type flags, throw TypeError if not.
///
/// \code
///   HermesBuiltin.checkedTypeCast = function(value, typeFlags) {...}
/// \endcode
CallResult<HermesValue> hermesBuiltinCheckedTypeCast(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  HermesValue value = args.getArg(0);
  uint16_t flags = static_cast<uint16_t>(args.getArg(1).getNumber());
  if (LLVM_LIKELY(matchTypeOfIs(value, TypeOfIsTypes(flags))))
    return value;
  return runtime.raiseTypeError("Checked cast failed");
}

/// Throw a reference error with the argument as a message.
///
/// \code
///   HermesBuiltin.throwReferenceError = function(errorMessage) {...}
/// \endcode
CallResult<HermesValue> hermesBuiltinThrowReferenceError(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return runtime.raiseReferenceError(args.getArgHandle(0));
}

namespace {

CallResult<HermesValue> copyDataPropertiesSlowPath_RJS(
    Runtime &runtime,
    Handle<JSObject> target,
    Handle<JSObject> from,
    Handle<JSObject> excludedItems) {
  struct : public Locals {
    PinnedValue<> nextKeyHandle;
    PinnedValue<> propValueHandle;
    PinnedValue<JSArray> keys;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 5. Let keys be ? from.[[OwnPropertyKeys]]().
  auto cr = JSObject::getOwnPropertyKeys(
      from,
      runtime,
      OwnKeysFlags()
          .plusIncludeSymbols()
          .plusIncludeNonSymbols()
          .plusIncludeNonEnumerable());
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.keys = std::move(*cr);

  // 6. For each element nextKey of keys in List order, do
  for (uint32_t nextKeyIdx = 0, endIdx = lv.keys->getEndIndex();
       nextKeyIdx < endIdx;
       ++nextKeyIdx) {
    lv.nextKeyHandle = lv.keys->at(runtime, nextKeyIdx).unboxToHV(runtime);
    if (lv.nextKeyHandle->isNumber()) {
      CallResult<PseudoHandle<StringPrimitive>> strRes =
          toString_RJS(runtime, lv.nextKeyHandle);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.nextKeyHandle = strRes->getHermesValue();
    }

    // b. For each element e of excludedItems in List order, do
    //   i. If SameValue(e, nextKey) is true, then
    //     1. Set excluded to true.
    if (excludedItems) {
      assert(
          !excludedItems->isProxyObject(runtime) &&
          "internal excludedItems object is a proxy");
      ComputedPropertyDescriptor desc;
      CallResult<bool> cr = JSObject::getOwnComputedPrimitiveDescriptor(
          excludedItems,
          runtime,
          lv.nextKeyHandle,
          JSObject::IgnoreProxy::Yes,
          desc);
      if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      if (*cr)
        continue;
    }

    //   i. Let desc be ? from.[[GetOwnProperty]](nextKey).
    ComputedPropertyDescriptor desc;
    CallResult<bool> crb = JSObject::getOwnComputedDescriptor(
        from, runtime, lv.nextKeyHandle, desc);
    if (LLVM_UNLIKELY(crb == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    //   ii. If desc is not undefined and desc.[[Enumerable]] is true, then
    // TODO(T141997867), move this special case behavior for host objects to
    // getOwnComputedDescriptor.
    if ((*crb && desc.flags.enumerable) || from->isHostObject(runtime)) {
      //     1. Let propValue be ? Get(from, nextKey).
      CallResult<PseudoHandle<>> crv =
          JSObject::getComputed_RJS(from, runtime, lv.nextKeyHandle);
      if (LLVM_UNLIKELY(crv == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      lv.propValueHandle = std::move(*crv);
      //     2. Perform ! CreateDataProperty(target, nextKey, propValue).
      crb = JSObject::defineOwnComputed(
          target,
          runtime,
          lv.nextKeyHandle,
          DefinePropertyFlags::getDefaultNewPropertyFlags(),
          lv.propValueHandle);
      if (LLVM_UNLIKELY(crb == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      assert(*crb && "CreateDataProperty failed");
    }
  }
  return target.getHermesValue();
}

} // namespace

/// \code
///   HermesBuiltin.copyDataProperties =
///         function (target, source, excludedItems) {}
/// \endcode
///
/// Copy all enumerable own properties of object \p source, that are not also
/// properties of \p excludedItems, into \p target, which must be an object, and
/// return \p target. If \p excludedItems is not specified, it is assumed
/// to be empty.
CallResult<HermesValue> hermesBuiltinCopyDataProperties(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<JSObject> source;
    PinnedValue<> nameHandle;
    PinnedValue<> valueHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 1. Assert: Type(target) is Object.
  Handle<JSObject> target = args.dyncastArg<JSObject>(0);
  // To be safe, ignore non-objects.
  if (!target)
    return HermesValue::encodeUndefinedValue();

  // 3. If source is undefined or null, return target.
  Handle<> untypedSource = args.getArgHandle(1);
  if (untypedSource->isNull() || untypedSource->isUndefined())
    return target.getHermesValue();

  // 4. Let from be ! ToObject(source).
  Handle<JSObject> source = untypedSource->isObject()
      ? Handle<JSObject>::vmcast(untypedSource)
      : [&]() {
          lv.source.castAndSetHermesValue<JSObject>(
              *toObject(runtime, untypedSource));
          return Handle<JSObject>{lv.source};
        }();

  // 2. Assert: excludedItems is a List of property keys.
  // In Hermes, excludedItems is represented as a JSObject, created by
  // bytecode emitted by the compiler, whose keys are the excluded
  // propertyKeys
  Handle<JSObject> excludedItems = args.dyncastArg<JSObject>(2);
  assert(
      (!excludedItems || !excludedItems->isProxyObject(runtime)) &&
      "excludedItems internal List is a Proxy");

  // We cannot use the fast path if the object is a proxy, host object, or when
  // there could potentially be an accessor defined on the object. This is
  // because in order to use JSObject::forEachOwnPropertyWhile, we must not
  // modify the underlying property map or hidden class. However, if we have an
  // accessor, we cannot guarantee that condition, so we use the slow path.
  if (source->isProxyObject(runtime) || source->isHostObject(runtime) ||
      source->getClass(runtime)->getMayHaveAccessor()) {
    return copyDataPropertiesSlowPath_RJS(
        runtime, target, source, excludedItems);
  }

  // Process all named properties/symbols.
  bool success = JSObject::forEachOwnPropertyWhile(
      source,
      runtime,
      // indexedCB.
      [&source, &target, &excludedItems, &lv](
          Runtime &runtime, uint32_t index, ComputedPropertyDescriptor desc) {
        if (!desc.flags.enumerable)
          return true;

        lv.nameHandle = HermesValue::encodeTrustedNumberValue(index);

        if (excludedItems) {
          assert(
              !excludedItems->isProxyObject(runtime) &&
              "internal excludedItems object is a proxy");
          ComputedPropertyDescriptor xdesc;
          auto cr = JSObject::getOwnComputedPrimitiveDescriptor(
              excludedItems,
              runtime,
              lv.nameHandle,
              JSObject::IgnoreProxy::Yes,
              xdesc);
          if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
            return false;
          if (*cr)
            return true;
        }

        lv.valueHandle = JSObject::getOwnIndexed(
            createPseudoHandle(source.get()), runtime, index);

        if (LLVM_UNLIKELY(
                JSObject::defineOwnComputedPrimitive(
                    target,
                    runtime,
                    lv.nameHandle,
                    DefinePropertyFlags::getDefaultNewPropertyFlags(),
                    lv.valueHandle) == ExecutionStatus::EXCEPTION)) {
          return false;
        }

        return true;
      },
      // namedCB.
      [&source, &target, &excludedItems, &lv](
          Runtime &runtime, SymbolID sym, NamedPropertyDescriptor desc) {
        if (!desc.flags.enumerable)
          return true;
        if (InternalProperty::isInternal(sym))
          return true;

        // Skip excluded items.
        if (excludedItems) {
          auto cr = JSObject::hasNamedOrIndexed(excludedItems, runtime, sym);
          assert(
              cr != ExecutionStatus::EXCEPTION &&
              "hasNamedOrIndex failed, which can only happen with a proxy, "
              "but excludedItems should never be a proxy");
          if (*cr)
            return true;
        }

        SmallHermesValue shv =
            JSObject::getNamedSlotValueUnsafe(*source, runtime, desc);
        lv.valueHandle = shv.unboxToHV(runtime);

        // sym can be an index-like property, so we have to bypass the assert in
        // defineOwnPropertyInternal.
        if (LLVM_UNLIKELY(
                JSObject::defineOwnPropertyInternal(
                    target,
                    runtime,
                    sym,
                    DefinePropertyFlags::getDefaultNewPropertyFlags(),
                    lv.valueHandle) == ExecutionStatus::EXCEPTION)) {
          return false;
        }

        return true;
      });

  if (LLVM_UNLIKELY(!success))
    return ExecutionStatus::EXCEPTION;

  return target.getHermesValue();
}

/// \code
///   HermesBuiltin.copyRestArgsFast = function (from) {}
/// \endcode
/// Same as copyRestArgs, but produces a FastArray. Used by typed-mode rest
/// parameters declared as Array<T>, where the consuming code expects a
/// FastArray rather than an ordinary JSArray.
CallResult<HermesValue> hermesBuiltinCopyRestArgsFast(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<FastArray> array;
    PinnedValue<> val;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto frames = runtime.getStackFrames();
  auto it = frames.begin();
  ++it;
  if (LLVM_UNLIKELY(it == frames.end()))
    return HermesValue::encodeUndefinedValue();

  if (!args.getArg(0).isNumber())
    return HermesValue::encodeUndefinedValue();
  uint32_t from = truncateToUInt32(args.getArg(0).getNumber());

  uint32_t argCount = it->getArgCount();
  uint32_t length = from <= argCount ? argCount - from : 0;

  auto cr = FastArray::create(runtime, length);
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  lv.array.castAndSetHermesValue<FastArray>(*cr);

  for (uint32_t i = 0; i != length; ++i) {
    lv.val = it->getArgRef(from);
    if (LLVM_UNLIKELY(
            FastArray::push(lv.array, runtime, lv.val) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    ++from;
  }

  return lv.array.getHermesValue();
}

/// \code
///   HermesBuiltin.copyRestArgs = function (from) {}
/// \endcode
/// Copy the callers parameters starting from index \c from (where the first
/// parameter is index 0) into a JSArray.
CallResult<HermesValue> hermesBuiltinCopyRestArgs(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<JSArray> array;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Obtain the caller's stack frame.
  auto frames = runtime.getStackFrames();
  auto it = frames.begin();
  ++it;
  // Check for the extremely unlikely case where there is no caller frame.
  if (LLVM_UNLIKELY(it == frames.end()))
    return HermesValue::encodeUndefinedValue();

  // "from" should be a number.
  if (!args.getArg(0).isNumber())
    return HermesValue::encodeUndefinedValue();
  uint32_t from = truncateToUInt32(args.getArg(0).getNumber());

  uint32_t argCount = it->getArgCount();
  uint32_t length = from <= argCount ? argCount - from : 0;

  auto cr = JSArray::create(runtime, length, length);
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  lv.array = std::move(*cr);
  JSArray::setStorageEndIndex(lv.array, runtime, length);

  for (uint32_t i = 0; i != length; ++i) {
    const auto shv =
        SmallHermesValue::encodeHermesValue(it->getArgRef(from), runtime);
    JSArray::unsafeSetExistingElementAt(*lv.array, runtime, i, shv);
    ++from;
  }

  return lv.array.getHermesValue();
}

/// \code
///   HermesBuiltin.arraySpread = function(target, source, nextIndex) {}
/// /endcode
/// ES9.0 12.2.5.2
/// Iterate the iterable source (as if using a for-of) and copy the values from
/// the spread source into the target array, starting at `nextIndex`. Then set
/// the length of the target array to the next empty index.
/// \return the next empty index in the array to use for additional properties.
CallResult<HermesValue> hermesBuiltinArraySpread(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<JSObject> next;
    PinnedValue<> nextValue;
    PinnedValue<> idxHandle;
    CheckedIteratorRecordStorage iterStorage;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  Handle<JSArray> target = args.dyncastArg<JSArray>(0);
  // To be safe, check for non-arrays.
  if (!target) {
    return runtime.raiseTypeError(
        "HermesBuiltin.arraySpread requires an array target");
  }

  Handle<JSArray> arr = args.dyncastArg<JSArray>(1);
  if (arr) {
    // Copying from an array, first check and make sure that
    // `arr[Symbol.iterator]` hasn't been changed by the user.
    NamedPropertyDescriptor desc;
    PseudoHandle<JSObject> propObj = createPseudoHandle(
        JSObject::getNamedDescriptorPredefined(
            arr, runtime, Predefined::SymbolIterator, desc));
    if (LLVM_LIKELY(propObj) && LLVM_LIKELY(!desc.flags.proxyObject)) {
      SmallHermesValue slotValue =
          JSObject::getNamedSlotValueUnsafe(propObj.get(), runtime, desc);
      propObj.invalidate();
      if (LLVM_LIKELY(
              slotValue.isObject() &&
              slotValue.getObject(runtime) == *runtime.arrayPrototypeValues)) {
        auto nextIndex = args.getArg(2).getNumberAs<JSArray::size_type>();
        for (JSArray::size_type i = 0; i < JSArray::getLength(*arr, runtime);
             ++i) {
          // Fast path: look up the property in indexed storage.
          lv.nextValue = arr->at(runtime, i).unboxToHV(runtime);
          if (LLVM_UNLIKELY(lv.nextValue->isEmpty())) {
            // Slow path, just run the full getComputed_RJS path.
            // Runs when there is a hole, accessor, non-regular property, etc.
            lv.idxHandle = HermesValue::encodeTrustedNumberValue(i);
            CallResult<PseudoHandle<>> valueRes =
                JSObject::getComputed_RJS(arr, runtime, lv.idxHandle);
            if (LLVM_UNLIKELY(valueRes == ExecutionStatus::EXCEPTION)) {
              return ExecutionStatus::EXCEPTION;
            }
            lv.nextValue = std::move(*valueRes);
          }
          // It is valid to use setElementAt here because we know that
          // `target` was created immediately prior to running the spread
          // and no non-standard properties were added to it,
          // because the only actions that can be performed between array
          // creation and running this spread are DefineOwnProperty calls with
          // standard flags (as well as other spread operations, which do the
          // same thing).
          if (LLVM_UNLIKELY(
                  JSArray::setElementAt(
                      target, runtime, nextIndex, lv.nextValue) ==
                  ExecutionStatus::EXCEPTION))
            return ExecutionStatus::EXCEPTION;
          ++nextIndex;
        }

        if (LLVM_UNLIKELY(
                JSArray::setLengthProperty(target, runtime, nextIndex) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }

        return HermesValue::encodeTrustedNumberValue(nextIndex);
      }
    }
  }

  // 3. Let iteratorRecord be ? GetIterator(spreadObj).
  auto iteratorRecordRes =
      getCheckedIterator(runtime, args.getArgHandle(1), lv.iterStorage);
  if (LLVM_UNLIKELY(iteratorRecordRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  CheckedIteratorRecord iteratorRecord = *iteratorRecordRes;

  auto nextIndex = args.getArg(2).getNumberAs<JSArray::size_type>();

  // 4. Repeat,
  for (;;) {
    // a. Let next be ? IteratorStep(iteratorRecord).
    auto nextRes = iteratorStep(runtime, iteratorRecord);
    if (LLVM_UNLIKELY(nextRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // b. If next is false, return nextIndex.
    if (!*nextRes) {
      // Set the length even if nothing was copied, so that it also covers any
      // elisions right before the spread.
      if (LLVM_UNLIKELY(
              JSArray::setLengthProperty(target, runtime, nextIndex) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      return HermesValue::encodeTrustedNumberValue(nextIndex);
    }
    lv.next = std::move(*nextRes);
    // c. Let nextValue be ? IteratorValue(next).
    auto nextItemRes = JSObject::getNamed_RJS(
        lv.next, runtime, Predefined::getSymbolID(Predefined::value));
    if (LLVM_UNLIKELY(nextItemRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.nextValue = std::move(*nextItemRes);

    // d. Let status be CreateDataProperty(array,
    //    ToString(ToUint32(nextIndex)), nextValue).
    // e. Assert: status is true.
    // setElementAt is valid here for the same reason as in the fast path. It
    // doesn't update the length, which is set once the iterator is done.
    if (LLVM_UNLIKELY(
            JSArray::setElementAt(target, runtime, nextIndex, lv.nextValue) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // f. Let nextIndex be nextIndex + 1.
    ++nextIndex;
  }
}

/// \code
///   HermesBuiltin.apply = function(fn, argArray, thisVal(opt)) {}
/// /endcode
/// Faster version of Function.prototype.apply which does not use its `this`
/// argument.
/// `argArray` must be a JSArray with no getters.
/// Equivalent to fn.apply(thisVal, argArray) if thisVal is provided.
/// If thisVal is not provided, equivalent to running `new fn` and passing the
/// arguments in argArray.
CallResult<HermesValue> hermesBuiltinApply(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<> thisVal;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  Handle<Callable> fn = args.dyncastArg<Callable>(0);
  if (LLVM_UNLIKELY(!fn)) {
    return runtime.raiseTypeErrorForValue(
        args.getArgHandle(0), " is not a function");
  }

  Handle<JSArray> argArray = args.dyncastArg<JSArray>(1);
  if (LLVM_UNLIKELY(!argArray)) {
    return runtime.raiseTypeError("args must be an array");
  }

  uint32_t len = JSArray::getLength(*argArray, runtime);

  bool isConstructor = args.getArgCount() == 2;
  if (isConstructor) {
    auto thisValRes = Callable::createThisForConstruct_RJS(fn, runtime, fn);
    if (LLVM_UNLIKELY(thisValRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.thisVal = thisValRes->getHermesValue();
  } else {
    lv.thisVal = args.getArg(2);
  }

  ScopedNativeCallFrame newFrame{
      runtime, len, *fn, isConstructor, lv.thisVal.getHermesValue()};
  if (LLVM_UNLIKELY(newFrame.overflowed()))
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);

  for (uint32_t i = 0; i < len; ++i) {
    assert(!argArray->at(runtime, i).isEmpty() && "arg array must be dense");
    HermesValue arg = argArray->at(runtime, i).unboxToHV(runtime);
    newFrame->getArgRef(i) = LLVM_UNLIKELY(arg.isEmpty())
        ? HermesValue::encodeUndefinedValue()
        : arg;
  }
  if (isConstructor) {
    auto res = Callable::construct(fn, runtime, lv.thisVal);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return res->getHermesValue();
  }
  auto res = Callable::call(fn, runtime);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return res->getHermesValue();
}

/// \code
///   HermesBuiltin.applyArguments = function(fn, thisVal, newTarget) {}
/// /endcode
/// Faster version of Function.prototype.apply which copies the arguments
/// from the caller to the callee.
CallResult<HermesValue> hermesBuiltinApplyArguments(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // Copy 'arguments' from the caller's stack, then call the callee.

  Handle<Callable> fn = args.dyncastArg<Callable>(0);
  if (LLVM_UNLIKELY(!fn)) {
    return runtime.raiseTypeErrorForValue(
        args.getArgHandle(0), " is not a function");
  }

  Handle<> newTarget = args.getArgHandle(2);
  bool isConstructCall = !newTarget->isUndefined();
  assert(
      (newTarget->isUndefined() || isConstructor(runtime, *newTarget)) &&
      "new.target can only be undefined or a constructor.");

  Handle<> thisHandle = args.getArgHandle(1);

  // Obtain the caller's stack frame.
  auto frames = runtime.getStackFrames();
  auto it = frames.begin();
  ++it;
  // Check for the extremely unlikely case where there is no caller frame.
  if (LLVM_UNLIKELY(it == frames.end()))
    return HermesValue::encodeUndefinedValue();

  uint32_t argCount = it->getArgCount();

  ScopedNativeCallFrame newFrame{
      runtime,
      argCount,
      HermesValue::encodeObjectValue(*fn),
      *newTarget,
      *thisHandle};
  if (LLVM_UNLIKELY(newFrame.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }

  for (uint32_t i = 0; i < argCount; ++i) {
    newFrame->getArgRef(i) = it->getArgRef(i);
  }

  if (isConstructCall) {
    return Callable::construct(fn, runtime, thisHandle)
        .toCallResultHermesValue();
  } else {
    return Callable::call(fn, runtime).toCallResultHermesValue();
  }
}

/// \code
///   HermesBuiltin.applyWithNewTarget = function(fn, argArray, thisVal,
///   newTarget) {}
/// /endcode
/// Perform a construct call on fn, with newTarget being set as the new.target.
/// This is only used in cases where the new.target is *not* implicitly set to
/// fn, as in the case of a new call. Thus, a direct `super` call may result in
/// this function being invoked.
/// `argArray` must be a JSArray with no getters.
CallResult<HermesValue> hermesBuiltinApplyWithNewTarget(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  assert(
      args.getArgCount() == 4 &&
      "builtinApplyWithNewTarget expected 4 arguments");

  Handle<Callable> fn = args.dyncastArg<Callable>(0);
  if (LLVM_UNLIKELY(!fn)) {
    return runtime.raiseTypeErrorForValue(
        args.getArgHandle(0), " is not a function");
  }

  Handle<JSArray> argArray = args.dyncastArg<JSArray>(1);
  if (LLVM_UNLIKELY(!argArray)) {
    return runtime.raiseTypeError("args must be an array");
  }

  uint32_t len = JSArray::getLength(*argArray, runtime);
  auto thisVal = args.getArgHandle(2);
  auto newTarget = args.getArgHandle(3);

  ScopedNativeCallFrame newFrame{
      runtime, len, HermesValue::encodeObjectValue(*fn), *newTarget, *thisVal};
  if (LLVM_UNLIKELY(newFrame.overflowed()))
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);

  for (uint32_t i = 0; i < len; ++i) {
    assert(!argArray->at(runtime, i).isEmpty() && "arg array must be dense");
    HermesValue arg = argArray->at(runtime, i).unboxToHV(runtime);
    newFrame->getArgRef(i) = LLVM_UNLIKELY(arg.isEmpty())
        ? HermesValue::encodeUndefinedValue()
        : arg;
  }
  auto res = Callable::construct(fn, runtime, thisVal);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return res->getHermesValue();
}

/// HermesBuiltin.exportAll(exports, source) will copy exported named
/// properties from `source` to `exports`, defining them on `exports` as
/// non-configurable.
/// Note that the default exported property on `source` is ignored,
/// as are non-enumerable properties on `source`.
CallResult<HermesValue> hermesBuiltinExportAll(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<> propertyHandle;
    PinnedValue<HiddenClass> sourceClass;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  Handle<JSObject> exports = args.dyncastArg<JSObject>(0);
  if (LLVM_UNLIKELY(!exports)) {
    return runtime.raiseTypeError(
        "exportAll() exports argument must be object");
  }

  Handle<JSObject> source = args.dyncastArg<JSObject>(1);
  if (LLVM_UNLIKELY(!source) || LLVM_UNLIKELY(source->isProxyObject(runtime))) {
    return runtime.raiseTypeError(
        "exportAll() source argument must be non-Proxy object");
  }

  auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.configurable = 0;

  CallResult<bool> defineRes{ExecutionStatus::EXCEPTION};

  // Iterate the named properties excluding those which use Symbols.
  lv.sourceClass.castAndSetHermesValue<HiddenClass>(
      HermesValue::encodeObjectValue(source->getClass(runtime)));
  bool result = HiddenClass::forEachPropertyWhile(
      lv.sourceClass,
      runtime,
      [&source, &exports, &lv, &dpf, &defineRes](
          Runtime &runtime, SymbolID id, NamedPropertyDescriptor desc) {
        if (!desc.flags.enumerable)
          return true;

        if (id == Predefined::getSymbolID(Predefined::defaultExport)) {
          return true;
        }

        lv.propertyHandle =
            JSObject::getNamedSlotValueUnsafe(*source, runtime, desc)
                .unboxToHV(runtime);
        defineRes = JSObject::defineOwnProperty(
            exports, runtime, id, dpf, lv.propertyHandle);
        if (LLVM_UNLIKELY(defineRes == ExecutionStatus::EXCEPTION)) {
          return false;
        }

        return true;
      });
  if (LLVM_UNLIKELY(!result)) {
    return ExecutionStatus::EXCEPTION;
  }
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue> hermesBuiltinExponentiate(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<BigIntPrimitive> lhs;
    PinnedValue<BigIntPrimitive> rhs;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  CallResult<HermesValue> res = toNumeric_RJS(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_LIKELY(!res->isBigInt())) {
    double left = res->getNumber();
    // Using ? toNumber() here causes an exception to be raised if args[1] is a
    // BigInt.
    CallResult<HermesValue> res = toNumber_RJS(runtime, args.getArgHandle(1));
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return HermesValue::encodeTrustedNumberValue(expOp(left, res->getNumber()));
  }

  lv.lhs.castAndSetHermesValue<BigIntPrimitive>(
      HermesValue::encodeBigIntValue(res->getBigInt()));

  // Can't use toBigInt() here as it converts boolean/strings to bigint.
  res = toNumeric_RJS(runtime, args.getArgHandle(1));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (!res->isBigInt()) {
    return runtime.raiseTypeErrorForValue(
        "Cannot convert ", args.getArgHandle(1), " to BigInt");
  }

  lv.rhs.castAndSetHermesValue<BigIntPrimitive>(
      HermesValue::encodeBigIntValue(res->getBigInt()));
  return BigIntPrimitive::exponentiate(runtime, lv.lhs, lv.rhs);
}

CallResult<HermesValue> hermesBuiltinInitRegexNamedGroups(
    void *ctx,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto *regexp = dyn_vmcast<JSRegExp>(args.getArg(0));
  auto *groupsObj = dyn_vmcast<JSObject>(args.getArg(1));
  regexp->setGroupNameMappings(runtime, groupsObj);
  return HermesValue::encodeUndefinedValue();
}

/// \code
///   HermesBuiltin.setFunctionName = function(F, name, prefix) {}
/// \endcode
/// This implements a subset of ES2025 10.2.9 SetFunctionName. This is only ever
/// used to initialize the names of methods and accessors for object literals
/// and classes.
/// \p F is the function object.
/// \p name is the property key. Only string and symbols are passed.
/// \p prefix is a number: 0 = no prefix, 1 = "get", 2 = "set".
CallResult<HermesValue> hermesBuiltinSetFunctionName(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<JSObject> F;
    PinnedValue<StringPrimitive> computedName;
    PinnedValue<StringPrimitive> result;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.F = vmcast<JSObject>(args.getArg(0));

  HermesValue nameArg = args.getArg(1);
  int prefix = 0;
  if (args.getArgCount() > 2 && args.getArg(2).isNumber())
    prefix = static_cast<int>(args.getArg(2).getNumber());

  // Do these spec steps out of order for better efficiency.
  ASCIIRef prefixStr{};
  // 5. If prefix is present, then
  // a. Set name to the string-concatenation of prefix, the code unit 0x0020
  // (SPACE), and name.
  if (prefix == 0) {
    prefixStr = createASCIIRef("");
  } else if (prefix == 1) {
    prefixStr = createASCIIRef("get ");
  } else if (prefix == 2) {
    prefixStr = createASCIIRef("set ");
  }
  SafeUInt32 len(prefixStr.size());

  bool needBrackets = false;
  // 2. If name is a Symbol, then
  if (nameArg.isSymbol()) {
    // a. Let description be name's [[Description]] value.
    auto *description = runtime.getStringPrimFromSymbolID(nameArg.getSymbol());
    // b. If description is undefined, set name to the empty String.
    if (description == runtime.strForSymbolNoDescription.get()) {
      lv.computedName = runtime.getPredefinedString(Predefined::emptyString);
    } else {
      // c. Else, set name to the string-concatenation of "[", description, and
      // "]".
      len.add(2);
      lv.computedName = description;
      needBrackets = true;
    }
  } else {
    // name is guaranteed to be a string.
    lv.computedName = vmcast<StringPrimitive>(nameArg);
  }

  len.add(lv.computedName->getStringLength());
  auto builderRes = StringBuilder::createStringBuilder(
      runtime, lv.result, len, lv.computedName->isASCII());
  if (LLVM_UNLIKELY(builderRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  auto builder = *builderRes;

  builder.appendASCIIRef(prefixStr);
  if (needBrackets)
    builder.appendCharacter('[');
  builder.appendStringPrim(lv.computedName);
  if (needBrackets)
    builder.appendCharacter(']');
  lv.computedName = *builder.getStringPrimitive();

  // Step 6: Define the "name" property with
  // { writable: false, enumerable: false, configurable: true }.
  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;

  auto defineRes = JSObject::defineOwnProperty(
      lv.F,
      runtime,
      Predefined::getSymbolID(Predefined::name),
      dpf,
      lv.computedName);
  if (LLVM_UNLIKELY(defineRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;

  return HermesValue::encodeUndefinedValue();
}

/// \code
///   HermesBuiltin.fastArrayPop = function (array, n) {}
/// \endcode
/// Pop the last \p n elements from a FastArray, returning the topmost popped
/// element or undefined if no element was popped. \p n is expected to be a
/// non-negative integral number; the SHBuiltin caller is responsible for
/// providing it. Values that don't fit in uint32_t are clamped to UINT32_MAX,
/// which will then be clamped to the array length by FastArray::pop.
CallResult<HermesValue> hermesBuiltinFastArrayPop(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto arr = Handle<FastArray>::vmcast(args.getArgHandle(0));
  double nDouble = args.getArg(1).getNumber();
  uint32_t n = nDouble >= (double)UINT32_MAX
      ? UINT32_MAX
      : (nDouble > 0 ? (uint32_t)nDouble : 0);
  return FastArray::pop(arr, runtime, n);
}

/// \code
///   HermesBuiltin.fastArraySlice = function (array, n) {}
/// \endcode
/// Return a new FastArray containing the elements of \p array starting at
/// index \p n (i.e., \c array.slice(n) for FastArrays). The result has the
/// same prototype as \p array. \p n is expected to be a non-negative
/// integral number; the IRGen caller is responsible for providing it.
/// Values that exceed the array length yield an empty array.
CallResult<HermesValue> hermesBuiltinFastArraySlice(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<FastArray> source;
    PinnedValue<JSObject> prototype;
    PinnedValue<FastArray> result;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.source = vmcast<FastArray>(args.getArg(0));
  double nDouble = args.getArg(1).getNumber();
  assert(
      nDouble >= 0 && nDouble == std::floor(nDouble) &&
      "fastArraySlice: n must be a non-negative integer");
  uint32_t n = nDouble >= (double)UINT32_MAX ? UINT32_MAX : (uint32_t)nDouble;

  uint32_t srcLen = lv.source->getLengthAsUint32(runtime);

  // Reuse the source's prototype so the result has the same Array<T>
  // class shape.
  lv.prototype = lv.source->getParent(runtime);

  // Clamp n to srcLen so FastArray::append's fromIndex is valid even when
  // the IRGen-supplied index exceeds the source length.
  uint32_t effN = std::min(n, srcLen);
  uint32_t resultLen = srcLen - effN;

  auto cr = FastArray::create(runtime, lv.prototype, resultLen);
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  lv.result.castAndSetHermesValue<FastArray>(*cr);

  if (LLVM_UNLIKELY(
          FastArray::append(lv.result, runtime, lv.source, effN) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;

  return lv.result.getHermesValue();
}

/// Typed Object.keys/values/entries: args are the object and the prototype of
/// the resulting Array<T>, supplied by IRGen.
/// @{
CallResult<HermesValue> hermesBuiltinObjectKeys(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return objectKeysFastArray(
      runtime,
      Handle<JSObject>::vmcast(args.getArgHandle(0)),
      Handle<JSObject>::vmcast(args.getArgHandle(1)));
}

CallResult<HermesValue> hermesBuiltinObjectValues(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return objectValuesFastArray(
      runtime,
      Handle<JSObject>::vmcast(args.getArgHandle(0)),
      Handle<JSObject>::vmcast(args.getArgHandle(1)));
}

CallResult<HermesValue> hermesBuiltinObjectEntries(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return objectEntriesFastArray(
      runtime,
      Handle<JSObject>::vmcast(args.getArgHandle(0)),
      Handle<JSObject>::vmcast(args.getArgHandle(1)));
}
/// @}

void createHermesBuiltins(Runtime &runtime) {
  struct : public Locals {
    PinnedValue<NativeFunction> method;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto defineInternMethod = [&](BuiltinMethod::Enum builtinIndex,
                                Predefined::Str symID,
                                NativeFunctionPtr func,
                                uint8_t count = 0) {
    lv.method = NativeFunction::create(
        runtime,
        nullptr /* context */,
        func,
        Predefined::getSymbolID(symID),
        count);
    runtime.registerBuiltin(builtinIndex, *lv.method);
  };

  // HermesBuiltin function properties
  namespace P = Predefined;
  namespace B = BuiltinMethod;
  defineInternMethod(
      B::HermesBuiltin_silentSetPrototypeOf,
      P::silentSetPrototypeOf,
      silentObjectSetPrototypeOf,
      2);
  defineInternMethod(
      B::HermesBuiltin_getTemplateObject,
      P::getTemplateObject,
      hermesBuiltinGetTemplateObject);
  defineInternMethod(
      B::HermesBuiltin_ensureObject,
      P::ensureObject,
      hermesBuiltinEnsureObject,
      2);
  defineInternMethod(
      B::HermesBuiltin_getMethod, P::getMethod, hermesBuiltinGetMethod, 2);
  defineInternMethod(
      B::HermesBuiltin_throwTypeError,
      P::throwTypeError,
      hermesBuiltinThrowTypeError,
      1);
  defineInternMethod(
      B::HermesBuiltin_throwReferenceError,
      P::throwReferenceError,
      hermesBuiltinThrowReferenceError,
      1);
  defineInternMethod(
      B::HermesBuiltin_copyDataProperties,
      P::copyDataProperties,
      hermesBuiltinCopyDataProperties,
      3);
  defineInternMethod(
      B::HermesBuiltin_copyRestArgs,
      P::copyRestArgs,
      hermesBuiltinCopyRestArgs,
      1);
  defineInternMethod(
      B::HermesBuiltin_copyRestArgsFast,
      P::copyRestArgsFast,
      hermesBuiltinCopyRestArgsFast,
      1);
  defineInternMethod(
      B::HermesBuiltin_arraySpread,
      P::arraySpread,
      hermesBuiltinArraySpread,
      2);
  defineInternMethod(B::HermesBuiltin_apply, P::apply, hermesBuiltinApply, 2);
  defineInternMethod(
      B::HermesBuiltin_applyArguments,
      P::apply,
      hermesBuiltinApplyArguments,
      2);
  defineInternMethod(
      B::HermesBuiltin_applyWithNewTarget,
      P::applyWithNewTarget,
      hermesBuiltinApplyWithNewTarget,
      4);
  defineInternMethod(
      B::HermesBuiltin_exportAll, P::exportAll, hermesBuiltinExportAll);
  defineInternMethod(
      B::HermesBuiltin_exponentiationOperator,
      P::exponentiationOperator,
      hermesBuiltinExponentiate);
  defineInternMethod(
      B::HermesBuiltin_initRegexNamedGroups,
      P::initRegexNamedGroups,
      hermesBuiltinInitRegexNamedGroups);
  defineInternMethod(
      B::HermesBuiltin_checkedTypeCast,
      P::checkedTypeCast,
      hermesBuiltinCheckedTypeCast,
      2);
  defineInternMethod(
      B::HermesBuiltin_setFunctionName,
      P::setFunctionName,
      hermesBuiltinSetFunctionName,
      3);

  defineInternMethod(
      B::HermesBuiltin_fastArrayPop,
      P::fastArrayPop,
      hermesBuiltinFastArrayPop,
      2);
  defineInternMethod(
      B::HermesBuiltin_fastArraySlice,
      P::fastArraySlice,
      hermesBuiltinFastArraySlice,
      2);

  defineInternMethod(
      B::HermesBuiltin_objectKeys, P::objectKeys, hermesBuiltinObjectKeys, 2);
  defineInternMethod(
      B::HermesBuiltin_objectValues,
      P::objectValues,
      hermesBuiltinObjectValues,
      2);
  defineInternMethod(
      B::HermesBuiltin_objectEntries,
      P::objectEntries,
      hermesBuiltinObjectEntries,
      2);

  // Define the 'requireFast' function, which takes a number argument.
  defineInternMethod(
      B::HermesBuiltin_requireFast, P::requireFast, requireFast, 1);
}

} // namespace vm
} // namespace hermes

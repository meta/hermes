/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES5.1 15.4 Initialize the Array constructor.
//===----------------------------------------------------------------------===//
#include "JSLibInternal.h"

#include "hermes/ADT/SafeInt.h"
#include "hermes/Support/FastArraySearch.h"
#include "hermes/VM/HandleRootOwner-inline.h"
#include "hermes/VM/JSLib.h"
#include "hermes/VM/JSLib/Sorting.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/SmallHermesValue.h"
#include "hermes/VM/StringBuilder.h"
#include "hermes/VM/StringRefUtils.h"
#include "hermes/VM/StringView.h"

#include "llvh/ADT/ScopeExit.h"

#include <algorithm>
#include <vector>

namespace hermes {
namespace vm {

//===----------------------------------------------------------------------===//
/// Array.

static inline CallResult<uint64_t>
lengthOfArrayLike(Runtime &runtime, Handle<JSObject> O, Handle<JSArray> jsArr) {
  if (LLVM_LIKELY(jsArr)) {
    // Fast path for getting the length.
    return JSArray::getLength(jsArr.get(), runtime);
  }
  struct : Locals {
    PinnedValue<> lenProp;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  // Slow path
  CallResult<PseudoHandle<>> propRes = JSObject::getNamed_RJS(
      O, runtime, Predefined::getSymbolID(Predefined::length));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  lv.lenProp = std::move(*propRes);
  auto lenRes = toLength(runtime, lv.lenProp);
  if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return lenRes->getNumber();
}

/// Try to perform the HasProperty(O, k) and Get(O, k) steps of the array
/// iteration methods without calling into JS. This succeeds if \p O is a
/// JSArray with fast index properties and its indexed storage holds a value
/// at index \p k. Such an element is an own data property, so it is present
/// and Get returns the stored value.
/// \p k must be below the length of \p O when the caller read it. A JSArray's
/// length is at most 2^32 - 1, so \p k then fits in the storage's index type.
/// This function does not allocate, so the caller can pass the result to a
/// function without rooting it, as long as nothing allocates in between.
/// \return the value of the element, or empty if the caller must use
///   getElementGeneric() instead, for example for a hole or a non-array \p O.
static inline HermesValue
tryGetFastArrayElement(Runtime &runtime, JSObject *O, uint64_t k) {
  auto *arr = dyn_vmcast<JSArray>(O);
  // An array without fast index properties has index-like named properties
  // (accessors, or elements with non-default attributes). Its non-empty slots
  // would still be own data properties, since no element is stored both in
  // the indexed storage and as a named property, but such arrays are rare, so
  // leave them to the generic lookup. The check only reads the hidden class,
  // and it matches the condition under which the generic lookup reads only the
  // indexed storage.
  if (LLVM_UNLIKELY(!arr || !arr->hasFastIndexProperties(runtime)))
    return HermesValue::encodeEmptyValue();
  assert(k < UINT32_MAX && "k must be below the length of a JSArray");
  // An empty slot (a hole) unboxes to empty.
  return arr->at(runtime, static_cast<uint32_t>(k)).unboxToHV(runtime);
}

/// Perform the HasProperty(O, k) and Get(O, k) steps of the array iteration
/// methods for any object \p O: look up property \p k on \p O and its
/// prototype chain, invoking getters and Proxy traps as needed. This is the
/// fallback for tryGetFastArrayElement(). It is not inlined, so that the
/// callers' loops stay small and do not reserve stack for its Locals.
/// The handles that the lookup allocates, for example for a getter or a Proxy
/// key, are released before returning. So they do not accumulate in the
/// caller's GCScope, even when a callback re-enters the caller through native
/// frames only, for example a forEach bound to an array.
/// \return the value of the property, or empty if it is not present. The
///   value is not rooted.
LLVM_ATTRIBUTE_NOINLINE
static CallResult<HermesValue>
getElementGeneric(Runtime &runtime, Handle<JSObject> O, uint64_t k) {
  struct : Locals {
    PinnedValue<> key;
    PinnedValue<JSObject> propObj;
    PinnedValue<SymbolID> tmpPropNameStorage;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  // Flushed on return. The result is a plain HermesValue, which stays valid
  // because flushing does not allocate.
  GCScopeMarkerRAII marker{runtime};

  lv.key = HermesValue::encodeTrustedNumberValue(k);
  ComputedPropertyDescWithSymStorage desc{lv.tmpPropNameStorage};
  if (LLVM_UNLIKELY(
          JSObject::getComputedPrimitiveDescriptor(
              O, runtime, lv.key, lv.propObj, desc) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return JSObject::getComputedPropertyValue_RJS(
             O, runtime, lv.propObj, desc.get(), lv.key)
      .toCallResultHermesValue();
}

/// Perform the HasProperty(O, k) and Get(O, k) steps of the array iteration
/// methods for any object \p O, using tryGetFastArrayElement() if possible and
/// getElementGeneric() otherwise.
/// \return the value of the property, or empty if it is not present. The
///   value is not rooted.
static inline CallResult<HermesValue>
getElementIfPresent(Runtime &runtime, Handle<JSObject> O, uint64_t k) {
  HermesValue value = tryGetFastArrayElement(runtime, *O, k);
  if (LLVM_LIKELY(!value.isEmpty()))
    return value;
  return getElementGeneric(runtime, O, k);
}

HermesValue createArrayConstructor(Runtime &runtime) {
  auto arrayPrototype = Handle<JSArray>::vmcast(&runtime.arrayPrototype);

  struct : public Locals {
    PinnedValue<NativeConstructor> cons;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Array.prototype.xxx methods.
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::toString),
      nullptr,
      arrayPrototypeToString,
      0);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::toLocaleString),
      nullptr,
      arrayPrototypeToLocaleString,
      0);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::at),
      nullptr,
      arrayPrototypeAt,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::concat),
      nullptr,
      arrayPrototypeConcat,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::join),
      nullptr,
      arrayPrototypeJoin,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::push),
      nullptr,
      arrayPrototypePush,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::sort),
      nullptr,
      arrayPrototypeSort,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::forEach),
      nullptr,
      arrayPrototypeForEach,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::flat),
      nullptr,
      arrayPrototypeFlat,
      0);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::flatMap),
      nullptr,
      arrayPrototypeFlatMap,
      1);

  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::keys),
      (void *)IterationKind::Key,
      arrayPrototypeIterator,
      0);
  runtime.arrayPrototypeValues = defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::values),
      (void *)IterationKind::Value,
      arrayPrototypeIterator,
      0);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::entries),
      (void *)IterationKind::Entry,
      arrayPrototypeIterator,
      0);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::toReversed),
      nullptr,
      arrayPrototypeToReversed,
      0);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::toSpliced),
      nullptr,
      arrayPrototypeToSpliced,
      2);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::toSorted),
      nullptr,
      arrayPrototypeToSorted,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::with),
      nullptr,
      arrayPrototypeWith,
      2);

  DefinePropertyFlags dpf = DefinePropertyFlags::getNewNonEnumerableFlags();

  runtime.ignoreAllocationFailure(
      JSObject::defineOwnProperty(
          arrayPrototype,
          runtime,
          Predefined::getSymbolID(Predefined::SymbolIterator),
          dpf,
          runtime.arrayPrototypeValues));

  defineSystemConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::Array),
      arrayConstructor,
      arrayPrototype,
      1,
      lv.cons);

  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::isArray),
      nullptr,
      arrayIsArray,
      1);

  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::slice),
      nullptr,
      arrayPrototypeSlice,
      2);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::splice),
      nullptr,
      arrayPrototypeSplice,
      2);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::copyWithin),
      nullptr,
      arrayPrototypeCopyWithin,
      2);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::pop),
      nullptr,
      arrayPrototypePop,
      0);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::shift),
      nullptr,
      arrayPrototypeShift,
      0);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::unshift),
      nullptr,
      arrayPrototypeUnshift,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::indexOf),
      nullptr,
      arrayPrototypeIndexOf,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::lastIndexOf),
      nullptr,
      arrayPrototypeLastIndexOf,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::every),
      nullptr,
      arrayPrototypeEvery,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::some),
      nullptr,
      arrayPrototypeSome,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::map),
      nullptr,
      arrayPrototypeMap,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::filter),
      nullptr,
      arrayPrototypeFilter,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::fill),
      nullptr,
      arrayPrototypeFill,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::find),
      nullptr,
      arrayPrototypeFind,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::findIndex),
      // Pass a non-null pointer here to indicate we're finding the index.
      (void *)true,
      arrayPrototypeFind,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::findLast),
      nullptr,
      arrayPrototypeFindLast,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::findLastIndex),
      // Pass a non-null pointer here to indicate we're finding the index.
      (void *)true,
      arrayPrototypeFindLast,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::reduce),
      nullptr,
      arrayPrototypeReduce,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::reduceRight),
      nullptr,
      arrayPrototypeReduceRight,
      1);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::reverse),
      nullptr,
      arrayPrototypeReverse,
      0);
  defineMethod(
      runtime,
      arrayPrototype,
      Predefined::getSymbolID(Predefined::includes),
      nullptr,
      arrayPrototypeIncludes,
      1);

  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::of),
      nullptr,
      arrayOf,
      0);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::from),
      nullptr,
      arrayFrom,
      1);

  return lv.cons.getHermesValue();
}

/// Populate \p flags with the flags to be used to check for fast path for array
/// methods.
/// Ensures that the HiddenClass has no behaviors that would make the array
/// fast path observe different indexed-property behavior.
static constexpr void setArrayFastPathClassFlags(ClassFlags &res) {
  // This code should set every field in ClassFlags to make sure we don't
  // miss one, which is checked by the assert in arrayFastPathCheck.
  res.dictionaryMode = 0;
  res.dictionaryNoCacheMode = 0;
  res.typed = 0;
  res.hasIndexLikeProperties = 0;
  res.indexedStorage = 1;
  res.mayHaveAccessor = 0;
  res.noExtend = 0;
  res.sealed = 0;
  res.frozen = 0;
  res.hostObject = 0;
  res.lazyObject = 0;
  res.proxyObject = 0;
  res.isCachedUsingEpoch = 0;
  res.parentChangeCounter = 0;
  res.unusedPadding = 0;
}

/// Check that the prototype chain of arrays starting at Array.prototype does
/// not contain any index-like properties. If it does not, update the epoch in
/// the runtime so that calling this function can be skipped when the epoch
/// matches. This is deliberately defined out-of-line because it is a slow path
/// and we don't want it to affect inlining of arrayFastPathCheck.
/// \return true if the prototype chain does not contain any index-like
/// properties, false otherwise.
static bool checkAndCacheProtoForFastPath(Runtime &runtime) {
  Handle<JSObject> arrParent = runtime.arrayPrototype;
  JSObject::setCachedUsingEpoch(arrParent, runtime);

  // If the parent has any index-like properties, bail.
  if (LLVM_UNLIKELY(arrParent->getClass(runtime)->getHasIndexLikeProperties()))
    return false;

  struct : public Locals {
    PinnedValue<JSObject> curParent;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // Check if there are any index-like properties in any parent.
  // If so, we can't use the fast path.
  for (lv.curParent = arrParent->getParent(runtime);
       lv.curParent.get() != nullptr;
       lv.curParent = lv.curParent->getParent(runtime)) {
    // If the object may have index-like properties that are not reflected on
    // the hidden class or indexed storage, bail.
    if (LLVM_UNLIKELY(
            lv.curParent->isHostObject(runtime) ||
            lv.curParent->isProxyObject(runtime) ||
            lv.curParent->isLazy(runtime)))
      return false;

    // Any index-like properties in the parent means we can't use the fast path
    // because we might trigger an accessor or have to check property flags.
    if (LLVM_UNLIKELY(
            lv.curParent->getClass(runtime)->getHasIndexLikeProperties()))
      return false;

    // Unlike the immediate parent, we do not expect any parent further up the
    // chain to have indexed storage. Disqualify anything with indexed storage,
    // since actually checking the "indexed range" is costly.
    if (LLVM_UNLIKELY(lv.curParent->hasIndexedStorage(runtime)))
      return false;

    JSObject::setCachedUsingEpoch(lv.curParent, runtime);
  }
  runtime.setArrayFastPathParentEpoch();

  return true;
}

/// Declared in JSLib.h so it can be used outside of JSLib
bool arrayFastPathCheck(
    Runtime &runtime,
    Handle<JSArray> arr,
    Handle<HiddenClass> arrayClass,
    uint32_t len) {
  {
    NoAllocScope noAlloc{runtime};
    assert(arr && "arr must be non-null");

    HiddenClass *clazz = arr->getClass(runtime);

    // Optionally check that 'length' hasn't been reconfigured.
    if (arrayClass && clazz != arrayClass.get())
      return false;

    // To use the fast path, the object has to be an array with fast index
    // properties (no index-like properties that we can't read quickly).
    // Make our own ClassFlags here to compare against quickly, to avoid having
    // to use lots of accessors on HiddenClass.
    ClassFlags arrayFastPathClassFlags{};
    setArrayFastPathClassFlags(arrayFastPathClassFlags);

#ifndef NDEBUG
    // Test that all the flags are handled in setArrayFastPathClassFlags
    // by starting with all the bits reversed and calling the function,
    // and making sure the result is the same.
    ClassFlags classFlagsForAssert{};
    std::memset(&classFlagsForAssert, 0xff, sizeof(ClassFlags));
    setArrayFastPathClassFlags(classFlagsForAssert);
    assert(
        std::memcmp(
            &classFlagsForAssert,
            &arrayFastPathClassFlags,
            sizeof(ClassFlags)) == 0 &&
        "setArrayFastPathClassFlags is missing a flag");
#endif

    ClassFlags arrClassFlags = clazz->getFlags();
    // Clear the flags that only affect named properties or caching, which the
    // fast path doesn't observe.
    arrClassFlags.dictionaryMode = 0;
    arrClassFlags.dictionaryNoCacheMode = 0;
    arrClassFlags.mayHaveAccessor = 0;
    arrClassFlags.typed = 0;
    arrClassFlags.isCachedUsingEpoch = 0;
    arrClassFlags.parentChangeCounter = 0;
    arrClassFlags.unusedPadding = 0;

    static_assert(
        sizeof(ClassFlags) == sizeof(uint16_t), "ClassFlags must be uint16_t");
    if (LLVM_UNLIKELY(
            std::memcmp(
                &arrayFastPathClassFlags, &arrClassFlags, sizeof(uint16_t)) !=
            0))
      return false;

    // Fast path assumes that the array storage goes from 0 to len.
    if (arr->getBeginIndex() != 0 || arr->getElemCount() != len)
      return false;

    JSArray *arrParent = *runtime.arrayPrototype;

    // If the parent has been modified, bail.
    if (LLVM_UNLIKELY(arr->getParent(runtime) != arrParent))
      return false;

    // If there are any indexed properties in the parent we can't use the fast
    // path. We check this specially because Array.prototype is itself
    // an array that starts with no actual indexed properties.
    //
    // NOTE: It may be possible to use the fast path if the indexed properties
    // don't overlap with the array's own indexed range, but that seems
    // unnecessary to check for now given that this check is really to make sure
    // that Array.prototype isn't filled with properties, and keeping track of
    // the upper bound on the length here takes more effort and is potentially
    // error-prone.
    if (LLVM_UNLIKELY(arrParent->getElemCount()))
      return false;
  }

  if (LLVM_UNLIKELY(!runtime.checkArrayFastPathParentEpoch()))
    return checkAndCacheProtoForFastPath(runtime);

  return true;
}

void arrayFastPathCopy(
    Runtime &runtime,
    Handle<JSArray> dest,
    Handle<JSArray> src,
    uint32_t len) {
  assert(
      arrayFastPathCheck(
          runtime, src, Runtime::makeNullHandle<HiddenClass>(), len) &&
      "src must pass arrayFastPathCheck");
  // An empty array may have no storage, and there is nothing to copy.
  if (len == 0)
    return;
  // dest is freshly created with the default class, so length is writable.
  // May allocate, so encode before NoAllocScope.
  auto newLen = SmallHermesValue::encodeNumberValue(len, runtime);
  NoAllocScope noAllocScope{runtime};
  auto *srcStorage = src->getIndexedStorageNullable(runtime);
  auto *destStorage = dest->getIndexedStorageNullable(runtime);
  assert(srcStorage && destStorage && "storage should not be null");
  destStorage->appendWithinCapacity(runtime, srcStorage);
  dest->setElemCountUnsafe(len);
  JSArray::putLengthUnsafe(dest.get(), runtime, newLen);
  // Replace holes (empty values) with undefined.
  for (uint32_t k = 0; k < len; ++k) {
    if (LLVM_UNLIKELY(destStorage->at(k).isEmpty())) {
      destStorage->set(
          k, SmallHermesValue::encodeUndefinedValue(), runtime.getHeap());
    }
  }
}

CallResult<HermesValue> arrayConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // NativeConstructors create their own this when called with new. The array
  // constructor also creates a new JSArray when it's called normally. So, we
  // will always create a new JSArray when called.
  struct : public Locals {
    PinnedValue<JSObject> selfParent;
    PinnedValue<JSArray> self;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  // If this is not a construct call, or it is a construct call and new.target
  // is the array constructor, then we know what parent to use to create the new
  // JSArray.
  if (LLVM_LIKELY(
          !args.isConstructorCall() ||
          (args.getNewTarget().getRaw() ==
           runtime.arrayConstructor.getHermesValue().getRaw()))) {
    CallResult<PseudoHandle<JSArray>> selfRes =
        JSArray::createNoAllocPropStorage(
            runtime, runtime.arrayPrototype, runtime.classJSArray);
    if (LLVM_UNLIKELY(selfRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.self = std::move(*selfRes);
  } else {
    CallResult<PseudoHandle<JSObject>> thisParentRes =
        NativeConstructor::parentForNewThis_RJS(
            runtime,
            Handle<Callable>::vmcast(&args.getNewTarget()),
            runtime.arrayPrototype);
    if (LLVM_UNLIKELY(thisParentRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.selfParent = std::move(*thisParentRes);
    auto arrRes = JSArray::create(runtime, lv.selfParent);
    if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.self = std::move(*arrRes);
  }

  // Possibility 1: new Array(number)
  if (args.getArgCount() == 1 && args.getArg(0).isNumber()) {
    double number = args.getArg(0).getNumber();
    uint32_t len = truncateToUInt32(number);
    if (len != number) {
      return runtime.raiseRangeError("invalid array length");
    }

    auto st = JSArray::setLengthProperty(lv.self, runtime, len);
    (void)st;
    assert(
        st != ExecutionStatus::EXCEPTION && *st &&
        "Cannot set length of a new array");

    return lv.self.getHermesValue();
  }

  // Possibility 2: new Array(elements...)
  uint32_t len = args.getArgCount();

  // Resize the array.
  auto st = JSArray::setLengthProperty(lv.self, runtime, len);
  (void)st;
  assert(
      st != ExecutionStatus::EXCEPTION && *st &&
      "Cannot set length of a new array");

  // Initialize the elements.
  uint32_t index = 0;
  GCScopeMarkerRAII marker(runtime);
  for (Handle<> arg : args.handles()) {
    if (LLVM_UNLIKELY(
            JSArray::setElementAt(lv.self, runtime, index++, arg) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    marker.flush();
  }

  return lv.self.getHermesValue();
}

CallResult<HermesValue> arrayIsArray(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  CallResult<bool> res = isArray(runtime, dyn_vmcast<JSObject>(args.getArg(0)));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return HermesValue::encodeBoolValue(*res);
}

/// ES5.1 15.4.4.5.
CallResult<HermesValue> arrayPrototypeToString(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : Locals {
    PinnedValue<JSObject> array;
    PinnedValue<Callable> func;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  lv.array.castAndSetHermesValue<JSObject>(objRes.getValue());

  auto propRes = JSObject::getNamed_RJS(
      lv.array, runtime, Predefined::getSymbolID(Predefined::join));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (vmisa<Callable>(propRes->getHermesValue())) {
    lv.func.castAndSetHermesValue<Callable>(propRes->getHermesValue());
    return Callable::executeCall0(lv.func, runtime, lv.array)
        .toCallResultHermesValue();
  } else {
    // If not callable, set func to be Object.prototype.toString.
    return directObjectPrototypeToString(runtime, lv.array);
  }
}

CallResult<HermesValue> arrayPrototypeToLocaleString(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  struct : Locals {
    PinnedValue<JSObject> array;
    PinnedValue<> E;
    PinnedValue<JSObject> elementObj;
    PinnedValue<> lenProp;
    PinnedValue<Callable> func;
    PinnedValue<JSArray> strings;
    PinnedValue<StringPrimitive> strElement;
    PinnedValue<> tmpValue;
    PinnedValue<StringPrimitive> emptyString;
    PinnedValue<StringPrimitive> result;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.array.castAndSetHermesValue<JSObject>(objRes.getValue());

  lv.emptyString = runtime.getPredefinedString(Predefined::emptyString);

  if (runtime.insertVisitedObject(*lv.array))
    return lv.emptyString.getHermesValue();
  auto cycleScope = llvh::make_scope_exit(
      [&lv, &runtime] { runtime.removeVisitedObject(*lv.array); });

  // 2. Let len be ? LengthOfArrayLike(O).
  auto lenRes = lengthOfArrayLike(
      runtime,
      lv.array,
      vmisa<JSArray>(*lv.array) ? Handle<JSArray>::vmcast(&lv.array)
                                : Runtime::makeNullHandle<JSArray>());
  if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t len = *lenRes;
  if (len == 0) {
    return lv.emptyString.getHermesValue();
  }
  // We use a JSArray to store intermediate strings, which is limited to
  // uint32_t elements.
  if (len > UINT32_MAX) {
    return runtime.raiseRangeError("invalid array length");
  }
  uint32_t len32 = static_cast<uint32_t>(len);

  // TODO: Get a list-separator String for the host environment's locale.
  // Use a comma as a separator for now, as JSC does.
  const char16_t separator = u',';

  // Final size of the result string. Initialize to account for the separators.
  SafeUInt32 size(len32 - 1);
  // Whether every element string is ASCII; the separator is.
  bool allASCII = true;

  // Array to store each of the strings of the elements.
  auto arrRes = JSArray::create(runtime, len32, len32);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.strings = std::move(*arrRes);

  auto marker = gcScope.createMarker();
  for (uint32_t i = 0; i < len32; ++i) {
    auto propRes = getIndexed_RJS(runtime, lv.array, i);
    gcScope.flushToMarker(marker);
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.E = std::move(*propRes);
    if (lv.E->isUndefined() || lv.E->isNull()) {
      // Empty string for undefined or null element. No need to add to size.
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(lv.strings, runtime, i, lv.emptyString) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
    } else {
      if (LLVM_UNLIKELY(
              (objRes = toObject(runtime, lv.E)) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.elementObj.castAndSetHermesValue<JSObject>(objRes.getValue());

      // Retrieve the toLocaleString function.
      if (LLVM_UNLIKELY(
              (propRes = JSObject::getNamed_RJS(
                   lv.elementObj,
                   runtime,
                   Predefined::getSymbolID(Predefined::toLocaleString))) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (vmisa<Callable>(propRes->get())) {
        lv.func.castAndSetHermesValue<Callable>(propRes->get());
        // If ECMA 402 is implemented, it provides a superseding
        // definition of Array.prototype.toLocaleString.  The only
        // difference between these two definitions is that in ECMA
        // 402, two arguments (locales and options), if provided, are
        // passed on from this function to the element's
        // "toLocaleString" method.
        auto callRes =
#ifdef HERMES_ENABLE_INTL
            Callable::executeCall2(
                lv.func,
                runtime,
                lv.elementObj,
                args.getArg(0),
                args.getArg(1));
#else
            Callable::executeCall0(lv.func, runtime, lv.elementObj);
#endif
        if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        lv.tmpValue = std::move(*callRes);
        auto strRes = toString_RJS(runtime, lv.tmpValue);
        if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        lv.strElement = std::move(*strRes);
        uint32_t strLength = lv.strElement->getStringLength();
        allASCII &= lv.strElement->isASCII();
        // Throw RangeError on overflow.
        size.add(strLength);
        if (LLVM_UNLIKELY(size.isOverflowed())) {
          return runtime.raiseRangeError(
              "resulting string length exceeds limit");
        }
        if (LLVM_UNLIKELY(
                JSArray::setElementAt(lv.strings, runtime, i, lv.strElement) ==
                ExecutionStatus::EXCEPTION))
          return ExecutionStatus::EXCEPTION;
      } else {
        return runtime.raiseTypeError("toLocaleString() not callable");
      }
    }
  }

  // Create and then populate the result string.
  auto builder =
      StringBuilder::createStringBuilder(runtime, lv.result, size, allASCII);
  if (builder == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.strElement = lv.strings->at(runtime, 0).getString(runtime);
  builder->appendStringPrim(lv.strElement);
  for (uint32_t j = 1; j < len32; ++j) {
    // Every element after the first needs a separator before it.
    builder->appendCharacter(separator);
    lv.strElement = lv.strings->at(runtime, j).getString(runtime);
    builder->appendStringPrim(lv.strElement);
  }
  return HermesValue::encodeStringValue(*builder->getStringPrimitive());
}

// 23.1.3.1
CallResult<HermesValue> arrayPrototypeAt(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> O;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  GCScope gcScope(runtime);
  // 1. Let O be ? ToObject(this value).
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*objRes);

  // 2. Let len be ? LengthOfArrayLike(O).
  auto lenRes = lengthOfArrayLike(
      runtime,
      lv.O,
      vmisa<JSArray>(*lv.O) ? Handle<JSArray>::vmcast(&lv.O)
                            : Runtime::makeNullHandle<JSArray>());
  if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto len = lenRes.getValue();

  // 3. Let relativeIndex be ? ToIntegerOrInfinity(index).
  auto idx = args.getArgHandle(0);
  auto relativeIndexRes = toIntegerOrInfinity(runtime, idx);
  if (relativeIndexRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  const double relativeIndex = relativeIndexRes->getNumber();

  double k;
  // 4. If relativeIndex ≥ 0, then
  if (relativeIndex >= 0) {
    // a. Let k be relativeIndex.
    k = relativeIndex;
  } else {
    // 5. Else,
    // a. Let k be len + relativeIndex.
    k = len + relativeIndex;
  }

  // 6. If k < 0 or k ≥ len, return undefined.
  if (k < 0 || k >= len) {
    return HermesValue::encodeUndefinedValue();
  }

  // 7. Return ? Get(O, ! ToString(𝔽(k))).
  CallResult<PseudoHandle<>> propRes =
      getIndexed_RJS(runtime, lv.O, (uint64_t)k);
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return propRes->getHermesValue();
}

CallResult<HermesValue> arrayPrototypeConcat(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<JSArray> A;
    PinnedValue<> k;
    PinnedValue<> n;
    PinnedValue<SymbolID> sym;
    PinnedValue<> tmp;
    PinnedValue<JSObject> obj;
    PinnedValue<JSObject> prop;
    PinnedValue<SymbolID> tmpPropNameStorage;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  GCScope gcScope(runtime);
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*objRes);

  // Need a signed type here to account for uint32 and -1.
  int64_t argCount = args.getArgCount();

  // Precompute the final size of the array so it can be preallocated.
  // Note this is necessarily an estimate because an accessor on one array
  // may change the length of subsequent arrays.
  SafeUInt32 finalSizeEstimate{0};
  if (vmisa<JSArray>(*lv.O)) {
    auto arr = vmcast<JSArray>(*lv.O);
    finalSizeEstimate.add(JSArray::getLength(arr, runtime));
  } else {
    finalSizeEstimate.add(1);
  }
  for (int64_t i = 0; i < argCount; ++i) {
    if (JSArray *arr = dyn_vmcast<JSArray>(args.getArg(i))) {
      finalSizeEstimate.add(JSArray::getLength(arr, runtime));
    } else {
      finalSizeEstimate.add(1);
    }
  }
  if (finalSizeEstimate.isOverflowed()) {
    return runtime.raiseTypeError("Array.prototype.concat result out of space");
  }

  // Resultant array.
  auto arrRes =
      JSArray::create(runtime, *finalSizeEstimate, *finalSizeEstimate);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.A = std::move(*arrRes);

  // Index to insert into A.
  uint64_t n = 0;
  auto marker = gcScope.createMarker();
  ComputedPropertyDescWithSymStorage desc{lv.tmpPropNameStorage};

  // Loop first through the "this" value and then through the arguments.
  // If i == -1, use the "this" value, else use the ith argument.
  lv.tmp = lv.O.getHermesValue();
  for (int64_t i = -1; i < argCount; ++i, lv.tmp = args.getArg(i)) {
    CallResult<bool> spreadable = isConcatSpreadable(runtime, lv.tmp);
    if (LLVM_UNLIKELY(spreadable == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (*spreadable) {
      // 7.d. If spreadable is true, then
      lv.obj.castAndSetHermesValue<JSObject>(*lv.tmp);
      bool isArray = vmisa<JSArray>(*lv.obj);
      uint64_t len;
      if (LLVM_LIKELY(isArray)) {
        auto *arr = vmcast<JSArray>(*lv.obj);
        // Fast path: E is an array.
        len = JSArray::getLength(arr, runtime);
      } else {
        CallResult<PseudoHandle<>> propRes = JSObject::getNamed_RJS(
            lv.obj, runtime, Predefined::getSymbolID(Predefined::length));
        if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        lv.tmp = std::move(*propRes);
        auto lengthRes = toLength(runtime, lv.tmp);
        if (LLVM_UNLIKELY(lengthRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        len = lengthRes->getNumberAs<uint64_t>();
      }

      // 5.c.iii. If n + len > 2^53 - 1, throw a TypeError exception
      if (LLVM_UNLIKELY(n + len > ((uint64_t)1 << 53) - 1)) {
        return runtime.raiseTypeError(
            "Array.prototype.concat result out of space");
      }

      // We know we are going to set elements in the range [n, n+len),
      // regardless of any changes to 'arrHandle' (see ES5.1 15.4.4.4). Ensure
      // we have capacity.
      if (LLVM_UNLIKELY(n + len > lv.A->getEndIndex()) &&
          LLVM_LIKELY(n + len < UINT32_MAX)) {
        // Only set the endIndex if it's going to be a valid length.
        if (LLVM_UNLIKELY(
                lv.A->setStorageEndIndex(lv.A, runtime, n + len) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
      }

      // Note that we must increase n every iteration even if nothing was
      // appended to the result array.
      // 5.c.iv. Repeat, while k < len
      for (uint64_t k = 0; k < len; ++k, ++n) {
        SmallHermesValue subElement = LLVM_LIKELY(isArray)
            ? vmcast<JSArray>(*lv.obj)->at(runtime, k)
            : SmallHermesValue::encodeEmptyValue();
        if (LLVM_LIKELY(!subElement.isEmpty()) &&
            LLVM_LIKELY(n < lv.A->getEndIndex())) {
          // Fast path: quickly set element without making any extra calls.
          // Cast is safe because A->getEndIndex must be in uint32_t range.
          JSArray::unsafeSetExistingElementAt(
              lv.A.get(), runtime, static_cast<uint32_t>(n), subElement);
        } else {
          // Slow path fallback if there's an empty slot in arr.
          // We have to use getComputedPrimitiveDescriptor because the property
          // may exist anywhere in the prototype chain.
          lv.k = HermesValue::encodeTrustedNumberValue(k);
          JSObject::getComputedPrimitiveDescriptor(
              lv.obj, runtime, lv.k, lv.prop, desc);
          CallResult<PseudoHandle<>> propRes =
              JSObject::getComputedPropertyValue_RJS(
                  lv.obj, runtime, lv.prop, desc.get(), lv.k);
          if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          if (LLVM_LIKELY(!(*propRes)->isEmpty())) {
            lv.tmp = std::move(*propRes);
            lv.n = HermesValue::encodeTrustedNumberValue(n);
            if (LLVM_UNLIKELY(
                    JSArray::defineOwnComputedPrimitive(
                        lv.A,
                        runtime,
                        lv.n,
                        DefinePropertyFlags::getDefaultNewPropertyFlags(),
                        lv.tmp) == ExecutionStatus::EXCEPTION)) {
              return ExecutionStatus::EXCEPTION;
            }
          }
          gcScope.flushToMarker(marker);
        }
      }
      gcScope.flushToMarker(marker);
    } else {
      // 5.d.i. NOTE: E is added as a single item rather than spread.
      // 5.d.ii. If n >= 2**53 - 1, throw a TypeError exception.
      if (LLVM_UNLIKELY(n >= ((uint64_t)1 << 53) - 1)) {
        return runtime.raiseTypeError(
            "Array.prototype.concat result out of space");
      }
      // Otherwise, just put the value into the next slot.
      if (LLVM_LIKELY(n < UINT32_MAX)) {
        if (LLVM_UNLIKELY(
                JSArray::setElementAt(lv.A, runtime, n, lv.tmp) ==
                ExecutionStatus::EXCEPTION))
          return ExecutionStatus::EXCEPTION;
      } else {
        lv.n = HermesValue::encodeTrustedNumberValue(n);
        auto cr = valueToSymbolID(runtime, lv.n, lv.sym);
        if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        if (LLVM_UNLIKELY(
                JSArray::defineOwnProperty(
                    lv.A,
                    runtime,
                    *lv.sym,
                    DefinePropertyFlags::getDefaultNewPropertyFlags(),
                    lv.tmp) == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
      }
      gcScope.flushToMarker(marker);
      ++n;
    }
  }
  // Update the array's length. We never expect this to fail since we just
  // created the array.
  if (n > UINT32_MAX) {
    return runtime.raiseRangeError("invalid array length");
  }
  auto res =
      JSArray::setLengthProperty(lv.A, runtime, static_cast<uint32_t>(n));
  assert(
      res == ExecutionStatus::RETURNED &&
      "Setting length of new array should never fail");
  (void)res;
  return lv.A.getHermesValue();
}

/// ES5.1 15.4.4.5.
CallResult<HermesValue> arrayPrototypeJoin(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<JSArray> arr;
    PinnedValue<> lenProp;
    PinnedValue<StringPrimitive> sep;
    PinnedValue<JSArray::StorageType> strings;
    PinnedValue<> elem;
    PinnedValue<StringPrimitive> elementStr;
    PinnedValue<JSArray::StorageType> inputStorage;
    PinnedValue<StringPrimitive> result;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  GCScope gcScope(runtime);
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*objRes);

  if (runtime.insertVisitedObject(*lv.O)) {
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::emptyString));
  }
  auto cycleScope = llvh::make_scope_exit(
      [&runtime, &lv] { runtime.removeVisitedObject(*lv.O); });

  // Obtain input length in len.
  auto propRes = JSObject::getNamed_RJS(
      lv.O, runtime, Predefined::getSymbolID(Predefined::length));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.lenProp = std::move(*propRes);
  auto intRes = toLengthU64(runtime, lv.lenProp);
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t len = *intRes;

  // Determine the separator. Use comma if the first argument is undefined.
  if (args.getArg(0).isUndefined()) {
    lv.sep = runtime.getPredefinedString(Predefined::comma);
  } else {
    // Otherwise convert it to string.
    auto strRes = toString_RJS(runtime, args.getArgHandle(0));
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    lv.sep = std::move(*strRes);
  }

  // Exit early if the input is empty.
  if (len == 0) {
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::emptyString));
  }

  // Track the size of the resultant string.
  SafeUInt32 size;
  // Whether the separator, if used, and every element string are ASCII.
  bool allASCII = len < 2 || lv.sep->isASCII();

  // The first element not processed by the fast path.
  uint32_t fastPathEnd = 0;

  // 1. Fast Path: Process as many elements as possible quickly.
  if (vmisa<JSArray>(lv.O.get()) &&
      arrayFastPathCheck(
          runtime,
          Handle<JSArray>::vmcast(&lv.O),
          Runtime::makeNullHandle<HiddenClass>(),
          (uint32_t)len)) {
    // Accumulate the size of the strings in the array, stopping at the first
    // element that is not a string, null, or undefined.
    lv.arr.castAndSetHermesValue<JSArray>(lv.O.getHermesValue());

    assert(
        len != 0 &&
        "we already checled len is not 0, so storage should be non-null");
    auto *storage = lv.arr->getIndexedStorageUnsafe(runtime);
    // Save it for later.
    lv.inputStorage = storage;

    uint32_t i;
    for (i = 0; i < len; ++i) {
      NoAllocScope noAlloc{runtime};
      SmallHermesValue elem = storage->at(i); // Direct access
      uint32_t elemLen;
      if (elem.isString()) {
        StringPrimitive *s = elem.getString(runtime);
        elemLen = s->getStringLength();
        allASCII &= s->isASCII();
      } else if (elem.isNull() || elem.isUndefined() || elem.isEmpty())
        elemLen = 0;
      else
        break;

      if (i > 0)
        size.add(lv.sep->getStringLength());
      size.add(elemLen);
    }
    fastPathEnd = i;
    // Check for string overflow to create the illusion that we are appending
    // to the string. Also, prevent uint32_t overflow.
    if (size.isOverflowed() ||
        size.get() > StringPrimitive::MAX_STRING_LENGTH) {
      return runtime.raiseRangeError("String is too long");
    }
  }

  // If there are remaining elements that weren't strings, or the input wasn't
  // a dense array at all.
  if (fastPathEnd < len) {
    // Create temporary storage for the remaining input strings. Element i of
    // lv.strings will correspond to element i of the input array. Elements
    // [0..fastPathEnd) will not be populated until we encounter an object
    // (since its toString has side effects).

    if (LLVM_UNLIKELY(len > UINT32_MAX))
      return runtime.raiseRangeError("Array is too large for join");
    auto arrRes =
        JSArray::StorageType::create(runtime, (uint32_t)len, (uint32_t)len);
    if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    lv.strings.castAndSetHermesValue<JSArray::StorageType>(*arrRes);

    auto marker = gcScope.createMarker();
    // Call toString on the remaining elements of the array.
    // i can't be larger than UINT32_MAX as checked above.
    for (uint32_t i = fastPathEnd; i < len;
         gcScope.flushToMarker(marker), ++i) {
      // Add the size of the separator, except the first time.
      if (i)
        size.add(lv.sep->getStringLength());

      // Fetch the element into lv.elem.
      if (lv.inputStorage.get()) {
        // Fast-path: input array is dense.
        lv.elem = lv.inputStorage->at(i).unboxToHV(runtime);
      } else {
        if (LLVM_UNLIKELY(
                (propRes = getIndexed_RJS(runtime, lv.O, i)) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        lv.elem = std::move(*propRes);
      }

      // null and undefined are empty strings. The empty could come from
      // reading the input storage directly; it acts as undefined.
      if (lv.elem->isUndefined() || lv.elem->isNull() || lv.elem->isEmpty()) {
        // Do nothing. Element in `strings` is already initialized to empty.
      } else {
        // Otherwise, call toString_RJS() and save the result, incrementing
        // size.

        // If the element is an object, converting it to a string can have
        // side effects and can modify the input array. We need to copy the
        // fast path elements and assume the input storage is no longer
        // accessible.
        if (lv.inputStorage.get() && lv.elem->isObject()) {
          assert(
              lv.inputStorage.get() &&
              "inputStorage should be non-null if fastPathEnd != 0");
          for (uint32_t j = 0; j < fastPathEnd; ++j) {
            SmallHermesValue elem = lv.inputStorage->at(j);
            if (elem.isString())
              lv.strings->set(j, elem, runtime.getHeap());
          }
          fastPathEnd = 0;
          lv.inputStorage = nullptr;
        }

        auto strRes = toString_RJS(runtime, lv.elem);
        if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION))
          return ExecutionStatus::EXCEPTION;

        StringPrimitive *s = strRes.getValue().get();
        size.add(s->getStringLength());
        allASCII &= s->isASCII();

        lv.strings->set(
            i,
            SmallHermesValue::encodeStringValue(s, runtime),
            runtime.getHeap());
      }

      // Check for string overflow on every iteration to create the illusion
      // that we are appending to the string. Also, prevent uint32_t overflow.
      if (size.isOverflowed() ||
          size.get() > StringPrimitive::MAX_STRING_LENGTH) {
        return runtime.raiseRangeError("String is too long");
      }
    }
  }

  // Allocate the complete result.
  auto builder =
      StringBuilder::createStringBuilder(runtime, lv.result, size, allASCII);
  if (builder == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  for (uint32_t i = 0; i < fastPathEnd; ++i) {
    if (i > 0)
      builder->appendStringPrim(lv.sep);
    SmallHermesValue elem = lv.inputStorage->at(i);
    if (elem.isString()) {
      lv.elementStr = lv.inputStorage->at(i).getString(runtime);
      builder->appendStringPrim(lv.elementStr);
    } else {
      assert(elem.isNull() || elem.isUndefined() || elem.isEmpty());
    }
  }

  for (uint32_t i = fastPathEnd; i < len; ++i) {
    if (i > 0)
      builder->appendStringPrim(lv.sep);
    auto hv = lv.strings->at(i);
    if (hv.isString()) {
      lv.elementStr = hv.getString(runtime);
      builder->appendStringPrim(lv.elementStr);
    } else {
      assert(hv.isEmpty() && "The element must be string or empty");
    }
  }

  return HermesValue::encodeStringValue(*builder->getStringPrimitive());
}

/// Fast path for Array.prototype.push() when the array is a normal array.
/// \pre \p arr is an array with fast index properties and there are no
///   index-like properties in any parents.
/// \pre \p The storage for \p arr starts at 0 and ends at its length.
/// \param arr the array to push onto.
/// \param len the length of \p arr (before pushing).
/// \param args the original NativeArgs to push().
static CallResult<HermesValue> arrayPrototypePushFastPath(
    Runtime &runtime,
    Handle<JSArray> arr,
    uint32_t len,
    const NativeArgs &args) {
  uint32_t argCount = args.getArgCount();

  assert(
      UINT32_MAX - len >= argCount &&
      "integer overflow checked before calling fast path");
  uint32_t finalLen = len + argCount;

  // Expand the array to make room for the new items.
  // Length property will be set at the end.
  if (LLVM_UNLIKELY(
          JSArray::increaseStorageEndIndex(arr, runtime, finalLen) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  for (uint32_t i = 0; i < argCount; ++i) {
    // Perform potential allocation before dereferencing arr.
    SmallHermesValue shv =
        SmallHermesValue::encodeHermesValue(args.getArg(i), runtime);
    JSArray::unsafeSetExistingElementAt(*arr, runtime, i + len, shv);
  }

  auto shv = SmallHermesValue::encodeNumberValue(finalLen, runtime);
  // Since we have already checked that the hidden class is unchanged, and
  // updated the storage end index, we can just directly store the new length to
  // the corresponding slot in the JSArray.
  JSArray::putLengthUnsafe(*arr, runtime, shv);

  return HermesValue::encodeTrustedNumberValue(finalLen);
}

/// Slow path for arrayPrototypePush.
/// Placed in a separate out-of-line function to reduce the size of the
/// entrypoint and fast path functions, leading to better codegen,
/// also to avoid allocating Locals in the fast path.
///
/// \param lenOpt if populated, we've already determined that the 'this' value
/// is a JSArray and retrieved the length.
LLVM_ATTRIBUTE_NOINLINE
static CallResult<HermesValue> arrayPrototypePushSlowPath(
    Runtime &runtime,
    OptValue<uint32_t> lenOpt) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  uint32_t argCount = args.getArgCount();

  // Ensure the fast path does not leak any handles.
  NoLeakHandleScope noLeaks{runtime};

  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<JSArray> arr;
    PinnedValue<> len;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // The slow path may create additional handles, so create a GCScope to avoid
  // leaking them.
  GCScope gcScope(runtime);

  // If the fast path did not populate the length, then get it the slow way.
  if (lenOpt) {
    lv.O.castAndSetHermesValue<JSObject>(args.getThisArg());
    lv.len = HermesValue::encodeTrustedNumberValue(*lenOpt);
  } else {
    // Slow path, used when pushing onto non-array objects.
    // 1. Let O be ? ToObject(this value).
    auto objRes = toObject(runtime, args.getThisHandle());
    if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.O.castAndSetHermesValue<JSObject>(*objRes);
    lv.arr = nullptr;
    auto propRes = JSObject::getNamed_RJS(
        lv.O, runtime, Predefined::getSymbolID(Predefined::length));
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.len = std::move(*propRes);
    auto lenRes = toLength(runtime, lv.len);
    if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.len = lenRes.getValue();
  }

  // 5. If len + argCount > 2**53-1, throw a TypeError exception.
  if (lv.len->getNumber() + (double)argCount > (1ULL << 53) - 1) {
    return runtime.raiseTypeError("Array length exceeded in push()");
  }

  auto marker = gcScope.createMarker();
  // 6. Repeat, while items is not empty
  for (auto arg : args.handles()) {
    // a. Remove the first element from items and let E be the value of the
    // element.
    // b. Perform ? Set(O, ! ToString(len), E, true).
    // NOTE: If the prototype has an index-like non-writable property at
    // index n, we have to fail to push.
    // If the prototype has an index-like accessor at index n,
    // then we have to attempt to call the setter.
    // Must call putComputed because the array prototype could have values for
    // keys that haven't been inserted into O yet.
    if (LLVM_UNLIKELY(
            JSObject::putComputed_RJS(
                lv.O, runtime, lv.len, arg, PropOpFlags().plusThrowOnError()) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    gcScope.flushToMarker(marker);
    // c. Let len be len+1.
    lv.len = HermesValue::encodeTrustedNumberValue(lv.len->getNumber() + 1);
  }

  // 7. Perform ? Set(O, "length", len, true).
  if (LLVM_UNLIKELY(
          JSObject::putNamed_RJS(
              lv.O,
              runtime,
              Predefined::getSymbolID(Predefined::length),
              lv.len,
              PropOpFlags().plusThrowOnError()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 8. Return len.
  return lv.len.get();
}

/// ES9.0 22.1.3.18.
CallResult<HermesValue> arrayPrototypePush(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  // Ensure the fast path does not leak any handles.
  NoLeakHandleScope noLeaks{runtime};

  // 3. Let items be a List whose elements are, in left to right order, the
  // arguments that were passed to this function invocation.
  // 4. Let argCount be the number of elements in items.
  uint32_t argCount = args.getArgCount();

  // 2. Let len be ? ToLength(? Get(O, "length")).
  OptValue<uint32_t> lenOpt = llvh::None;
  if (LLVM_LIKELY(vmisa<JSArray>(args.getThisArg()))) {
    // Fast path for getting the length.
    auto arr = args.vmcastThis<JSArray>();
    uint32_t len = JSArray::getLength(*arr, runtime);

    if (LLVM_LIKELY(len < UINT32_MAX - argCount) &&
        arrayFastPathCheck(runtime, arr, runtime.classJSArray, len)) {
      return arrayPrototypePushFastPath(
          runtime, args.vmcastThis<JSArray>(), len, args);
    }

    lenOpt = len;
  }

  return arrayPrototypePushSlowPath(runtime, lenOpt);
}

namespace {
/// A sort model that operates directly on a JSArray's indexed storage.
/// This is only safe when the caller owns the array and user code has no
/// reference to it (e.g. the fresh array created by toSorted). It bypasses
/// the property descriptor machinery and reads/writes via
/// storage->at()/storage->set() directly.
/// The array must not contain any holes (empty values). Its beginIndex must be
/// 0 and elements count must be equal to length.
/// Without a comparator, it caches ToString of the elements that are
/// primitives but not strings. ToString of a primitive has no side effects,
/// so converting it once is unobservable. Objects are converted on every
/// comparison, as the spec does.
class DirectSortModel : public SortModel {
 private:
  Runtime &runtime_;
  GCScope gcScope_;
  Handle<Callable> compareFn_;
  Handle<JSArray> arr_;

  struct : Locals {
    PinnedValue<> aValue;
    PinnedValue<> bValue;
    PinnedValue<> tmpValue;
    /// The cached ToString of the element at the same index of arr_, or
    /// empty. Created when the first one is cached.
    PinnedValue<JSArray> keys;
  } lv_;
  LocalsRAII lraii_;
  GCScope::Marker gcMarker_;

  /// Get the indexed storage, which must be re-fetched after any GC safepoint.
  JSArray::StorageType *storage() {
    return arr_->getIndexedStorageUnsafe(runtime_);
  }

  /// Swap the elements at \p a and \p b of \p arr.
  void swapElements(JSArray *arr, uint32_t a, uint32_t b) {
    auto *s = arr->getIndexedStorageUnsafe(runtime_);
    auto tmp = s->at(a);
    s->set(a, s->at(b), runtime_.getHeap());
    s->set(b, tmp, runtime_.getHeap());
  }

  /// \return ToString of \p value, the element at \p i, if it is \p value
  /// itself or cached, and null otherwise.
  StringPrimitive *knownString(uint32_t i, SmallHermesValue value) {
    if (value.isString())
      return value.getString(runtime_);
    if (!lv_.keys.get())
      return nullptr;
    SmallHermesValue key = lv_.keys->getIndexedStorageUnsafe(runtime_)->at(i);
    return key.isEmpty() ? nullptr : key.getString(runtime_);
  }

  /// Replace \p value, the element at \p i, with its ToString, and cache the
  /// result if \p value is a primitive.
  ExecutionStatus convertToString(uint32_t i, PinnedValue<> &value) {
    bool isPrimitive = !value->isObject();
    auto strRes = toString_RJS(runtime_, value);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    value = strRes->getHermesValue();
    if (!isPrimitive)
      return ExecutionStatus::RETURNED;
    if (!lv_.keys.get()) {
      uint32_t len = arr_->getElemCount();
      auto keysRes = JSArray::create(runtime_, len, len);
      if (LLVM_UNLIKELY(keysRes == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      lv_.keys = std::move(*keysRes);
    }
    lv_.keys->getIndexedStorageUnsafe(runtime_)->set(
        i,
        SmallHermesValue::encodeStringValue(value->getString(), runtime_),
        runtime_.getHeap());
    return ExecutionStatus::RETURNED;
  }

 public:
  DirectSortModel(
      Runtime &runtime,
      Handle<JSArray> arr,
      Handle<Callable> compareFn)
      : runtime_(runtime),
        gcScope_(runtime),
        compareFn_(compareFn),
        arr_(arr),
        lraii_(runtime_, &lv_),
        gcMarker_(gcScope_.createMarker()) {
    assert(arr_->getIndexedStorageUnsafe(runtime_) && "Empty array");
#ifndef NDEBUG
    NoAllocScope noAlloc{runtime};
    auto *s = arr->getIndexedStorageUnsafe(runtime);
    assert(
        (arr->getBeginIndex() == 0) &&
        (arr->getElemCount() == JSArray::getLength(*arr, runtime)));
    for (uint32_t i = 0, e = arr->getEndIndex(); i < e; ++i) {
      assert(
          !s->at(i).isEmpty() && "DirectSortModel input must not have holes");
    }
#endif
  }

  ExecutionStatus swap(uint32_t a, uint32_t b) override {
    NoAllocScope noAlloc{runtime_};
    swapElements(arr_.get(), a, b);
    if (lv_.keys.get())
      swapElements(lv_.keys.get(), a, b);
    return ExecutionStatus::RETURNED;
  }

  CallResult<int> compare(uint32_t a, uint32_t b) override {
    {
      NoAllocScope noAlloc{runtime_};
      auto *s = storage();
      auto aSmall = s->at(a);
      auto bSmall = s->at(b);
      if (aSmall.isUndefined()) {
        return bSmall.isUndefined() ? 0 : 1;
      }
      if (bSmall.isUndefined()) {
        return -1;
      }
      if (compareFn_) {
        lv_.aValue = aSmall.unboxToHV(runtime_);
        lv_.bValue = bSmall.unboxToHV(runtime_);
      } else {
        StringPrimitive *aStr = knownString(a, aSmall);
        StringPrimitive *bStr = knownString(b, bSmall);
        if (aStr && bStr)
          return aStr->compare(bStr);
        lv_.aValue = aStr ? HermesValue::encodeStringValue(aStr)
                          : aSmall.unboxToHV(runtime_);
        lv_.bValue = bStr ? HermesValue::encodeStringValue(bStr)
                          : bSmall.unboxToHV(runtime_);
      }
    }

    GCScopeMarkerRAII gcMarker{gcScope_, gcMarker_};
    if (compareFn_) {
      auto callRes = Callable::executeCall2(
          compareFn_,
          runtime_,
          Runtime::getUndefinedValue(),
          lv_.aValue.get(),
          lv_.bValue.get());
      if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv_.tmpValue = std::move(*callRes);
      auto intRes = toNumber_RJS(runtime_, lv_.tmpValue);
      if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      auto res = intRes->getNumber();
      return (res < 0) ? -1 : (res > 0 ? 1 : 0);
    } else {
      if (!lv_.aValue->isString() &&
          LLVM_UNLIKELY(
              convertToString(a, lv_.aValue) == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (!lv_.bValue->isString() &&
          LLVM_UNLIKELY(
              convertToString(b, lv_.bValue) == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      return lv_.aValue->getString()->compare(lv_.bValue->getString());
    }
  }
};

/// \return the number of elements of \p arr, an array of length \p len, that
/// are not holes.
/// \pre \p arr passes arrayFastPathCheck with length \p len.
uint32_t arrayFastPathCountNonHoles(
    Runtime &runtime,
    Handle<JSArray> arr,
    uint32_t len) {
  assert(
      arrayFastPathCheck(
          runtime, arr, Runtime::makeNullHandle<HiddenClass>(), len) &&
      "arr must pass arrayFastPathCheck");
  NoAllocScope noAlloc{runtime};
  auto *storage = arr->getIndexedStorageUnsafe(runtime);
  uint32_t count = 0;
  for (uint32_t k = 0; k < len; ++k)
    count += !storage->at(k).isEmpty();
  return count;
}

/// Copy the elements of \p src, an array of length \p len, into \p dest,
/// skipping holes. Since arrayFastPathCheck ensured that the prototype chain
/// has no indexed properties, a hole has no property, so
/// SortIndexedProperties skips it too.
/// \pre \p src passes arrayFastPathCheck with length \p len.
/// \pre \p dest is a new array from JSArray::create whose length is the
///   number of elements of \p src that are not holes.
void arrayFastPathCopySkipHoles(
    Runtime &runtime,
    Handle<JSArray> dest,
    Handle<JSArray> src,
    uint32_t len) {
  assert(
      arrayFastPathCheck(
          runtime, src, Runtime::makeNullHandle<HiddenClass>(), len) &&
      "src must pass arrayFastPathCheck");
  NoAllocScope noAlloc{runtime};
  auto *srcStorage = src->getIndexedStorageUnsafe(runtime);
  auto *destStorage = dest->getIndexedStorageUnsafe(runtime);
  uint32_t count = 0;
  for (uint32_t k = 0; k < len; ++k) {
    SmallHermesValue v = srcStorage->at(k);
    if (!v.isEmpty())
      destStorage->set(count++, v, runtime.getHeap());
  }
  assert(count == destStorage->size() && "dest has the wrong length");
}

/// Sort \p arr, an array of length \p len, as ES2025 23.1.3.30 does: copy the
/// elements into a list, sort the list, write it back, and delete the
/// remaining indices, which were holes.
/// \pre \p arr passes arrayFastPathCheck with length \p len.
ExecutionStatus arrayFastPathSort(
    Runtime &runtime,
    Handle<JSArray> arr,
    Handle<Callable> compareFn,
    uint32_t len) {
  assert(
      arrayFastPathCheck(
          runtime, arr, Runtime::makeNullHandle<HiddenClass>(), len) &&
      "arr must pass arrayFastPathCheck");
  struct : Locals {
    PinnedValue<JSArray> items;
    PinnedValue<> index;
    PinnedValue<> value;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  uint32_t count = arrayFastPathCountNonHoles(runtime, arr, len);
  // With only holes, there is nothing to sort or write back.
  if (count == 0)
    return ExecutionStatus::RETURNED;
  auto itemsRes = JSArray::create(runtime, count, count);
  if (LLVM_UNLIKELY(itemsRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  lv.items = std::move(*itemsRes);
  arrayFastPathCopySkipHoles(runtime, lv.items, arr, len);
  {
    DirectSortModel sm(runtime, lv.items, compareFn);
    if (LLVM_UNLIKELY(quickSort(&sm, 0u, count) == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }

  // The comparator may have changed arr, so check the fast path again. In
  // such an array, deleting an element leaves a hole.
  if (arrayFastPathCheck(
          runtime, arr, Runtime::makeNullHandle<HiddenClass>(), len)) {
    NoAllocScope noAlloc{runtime};
    auto *src = lv.items->getIndexedStorageUnsafe(runtime);
    auto *dest = arr->getIndexedStorageUnsafe(runtime);
    for (uint32_t k = 0; k < count; ++k)
      dest->set(k, src->at(k), runtime.getHeap());
    for (uint32_t k = count; k < len; ++k)
      dest->setNonPtr(
          k, SmallHermesValue::encodeEmptyValue(), runtime.getHeap());
    return ExecutionStatus::RETURNED;
  }

  for (uint32_t k = 0; k < len; ++k) {
    GCScopeMarkerRAII marker{runtime};
    lv.index = HermesValue::encodeTrustedNumberValue(k);
    if (k >= count) {
      if (LLVM_UNLIKELY(
              JSObject::deleteComputed(
                  arr, runtime, lv.index, PropOpFlags().plusThrowOnError()) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      continue;
    }
    lv.value = lv.items->at(runtime, k).unboxToHV(runtime);
    if (LLVM_UNLIKELY(
            JSObject::putComputed_RJS(
                arr,
                runtime,
                lv.index,
                lv.value,
                PropOpFlags().plusThrowOnError()) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }
  return ExecutionStatus::RETURNED;
}

/// \return whether \p obj or an object on its prototype chain is a proxy or a
/// host object. Only such objects can run code in [[HasProperty]] and
/// [[Delete]], or list their own properties incompletely.
bool hasProxyOrHostOnChain(Runtime &runtime, JSObject *obj) {
  NoAllocScope noAlloc{runtime};
  for (; obj; obj = obj->getParent(runtime)) {
    if (obj->isProxyObject(runtime) || obj->isHostObject(runtime))
      return true;
  }
  return false;
}

/// Append to \p keys, in ascending order, the indices in [\p from, \p len) of
/// the own properties of \p obj, listing all of its property names.
ExecutionStatus appendOwnIndexKeysFromNames(
    Runtime &runtime,
    Handle<JSObject> obj,
    uint32_t from,
    uint32_t len,
    std::vector<uint32_t> &keys) {
  GCScopeMarkerRAII marker{runtime};
  auto namesRes = JSObject::getOwnPropertyNames(obj, runtime, false);
  if (LLVM_UNLIKELY(namesRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  NoAllocScope noAlloc{runtime};
  auto *names = (*namesRes)->getIndexedStorageNullable(runtime);
  if (!names)
    return ExecutionStatus::RETURNED;
  // Index names come first, as numbers in ascending order.
  for (uint32_t i = 0, e = names->size(); i < e; ++i) {
    SmallHermesValue name = names->at(i);
    if (!name.isNumber())
      break;
    double index = name.getNumber(runtime);
    if (index >= len)
      break;
    if (index >= from)
      keys.push_back(static_cast<uint32_t>(index));
  }
  return ExecutionStatus::RETURNED;
}

/// Append to \p keys, in ascending order, the indices in [\p from, \p len) of
/// the own properties of \p obj.
/// \pre \p obj is not a proxy or a host object.
ExecutionStatus appendOwnIndexKeys(
    Runtime &runtime,
    Handle<JSObject> obj,
    uint32_t from,
    uint32_t len,
    std::vector<uint32_t> &keys) {
  // Only the indexed storage can hold indices, unless the class has a named
  // property with an index-like name.
  if (LLVM_UNLIKELY(obj->getClass(runtime)->getHasIndexLikeProperties()))
    return appendOwnIndexKeysFromNames(runtime, obj, from, len, keys);
  GCScopeMarkerRAII marker{runtime};
  JSObject::forEachOwnPropertyWhile(
      obj,
      runtime,
      [from, len, &keys](
          Runtime &, uint32_t index, ComputedPropertyDescriptor) {
        if (index >= len)
          return false;
        if (index >= from)
          keys.push_back(index);
        return true;
      },
      [](Runtime &, SymbolID, NamedPropertyDescriptor) { return false; });
  return ExecutionStatus::RETURNED;
}

/// Set \p keys to the indices below \p len, in ascending order, of the own
/// properties of \p obj and of the objects on its prototype chain.
/// \pre No object on the chain is a proxy or a host object.
ExecutionStatus collectIndexKeys(
    Runtime &runtime,
    Handle<JSObject> obj,
    uint32_t len,
    std::vector<uint32_t> &keys) {
  struct : Locals {
    PinnedValue<JSObject> cur;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  keys.clear();
  for (lv.cur = obj.get(); lv.cur.get(); lv.cur = lv.cur->getParent(runtime)) {
    if (LLVM_UNLIKELY(
            appendOwnIndexKeys(runtime, lv.cur, 0, len, keys) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }
  // Keys from different objects may interleave or repeat.
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  return ExecutionStatus::RETURNED;
}

/// If HasProperty(\p obj, \p index) is true, append Get(\p obj, \p index) to
/// \p items, whose length is \p count, and increment \p count.
/// \return whether a getter ran, which can change the elements.
CallResult<bool> readElementAt(
    Runtime &runtime,
    Handle<JSObject> obj,
    uint32_t index,
    Handle<JSArray> items,
    uint32_t &count) {
  struct : Locals {
    PinnedValue<> index;
    PinnedValue<> value;
    PinnedValue<JSObject> propObj;
    PinnedValue<SymbolID> tmpName;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScopeMarkerRAII marker{runtime};
  lv.index = HermesValue::encodeTrustedNumberValue(index);
  ComputedPropertyDescWithSymStorage desc{lv.tmpName};
  if (LLVM_UNLIKELY(
          JSObject::getComputedPrimitiveDescriptor(
              obj, runtime, lv.index, lv.propObj, desc) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  if (!lv.propObj.get())
    return false;
  auto valueRes = JSObject::getComputedPropertyValue_RJS(
      obj, runtime, lv.propObj, desc.get(), lv.index);
  if (LLVM_UNLIKELY(valueRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  // A proxy whose has trap returns false gives an empty value.
  if ((*valueRes)->isEmpty())
    return false;
  lv.value = std::move(*valueRes);
  if (LLVM_UNLIKELY(
          JSArray::setElementAt(items, runtime, count, lv.value) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  ++count;
  return desc.flags.accessor;
}

/// Read the elements of \p obj at every index in [\p from, \p len) into
/// \p items, whose length is \p count, as SortIndexedProperties does with
/// skip-holes.
ExecutionStatus readEveryIndex(
    Runtime &runtime,
    Handle<JSObject> obj,
    uint32_t from,
    uint32_t len,
    Handle<JSArray> items,
    uint32_t &count) {
  for (uint32_t k = from; k < len; ++k) {
    if (LLVM_UNLIKELY(
            readElementAt(runtime, obj, k, items, count) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }
  return ExecutionStatus::RETURNED;
}

/// Read the elements of \p obj below \p len into \p items, whose length is
/// \p count, visiting only the indices that have a property, own or
/// inherited. HasProperty runs no code on the other indices, since no object
/// on the prototype chain is a proxy or a host object. A getter can add or
/// delete elements, so the indices are collected again after one runs. Once
/// the collections have cost as much as visiting every index left, stop, so
/// the work stays linear in \p len when most elements are getters.
/// \return the index from which readEveryIndex must read the rest, or \p len
///   if all were read.
/// \pre No object on the prototype chain is a proxy or a host object.
CallResult<uint32_t> readExistingIndices(
    Runtime &runtime,
    Handle<JSObject> obj,
    uint32_t len,
    Handle<JSArray> items,
    uint32_t &count) {
  std::vector<uint32_t> keys;
  if (LLVM_UNLIKELY(
          collectIndexKeys(runtime, obj, len, keys) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  uint64_t collectionCost = 0;
  size_t i = 0;
  while (i < keys.size()) {
    uint32_t k = keys[i++];
    auto getterRanRes = readElementAt(runtime, obj, k, items, count);
    if (LLVM_UNLIKELY(getterRanRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    if (!*getterRanRes)
      continue;
    collectionCost += keys.size();
    if (collectionCost >= len - (k + 1) ||
        hasProxyOrHostOnChain(runtime, obj.get()))
      return k + 1;
    if (LLVM_UNLIKELY(
            collectIndexKeys(runtime, obj, len, keys) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    i = std::upper_bound(keys.begin(), keys.end(), k) - keys.begin();
  }
  return len;
}

/// Read the elements of \p obj below \p len into \p items, as
/// SortIndexedProperties does with skip-holes.
/// \return the number of elements read.
CallResult<uint32_t> readElements(
    Runtime &runtime,
    Handle<JSObject> obj,
    uint32_t len,
    Handle<JSArray> items) {
  uint32_t count = 0;
  uint32_t from = 0;
  if (!hasProxyOrHostOnChain(runtime, obj.get())) {
    auto fromRes = readExistingIndices(runtime, obj, len, items, count);
    if (LLVM_UNLIKELY(fromRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    from = *fromRes;
  }
  if (LLVM_UNLIKELY(
          readEveryIndex(runtime, obj, from, len, items, count) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return count;
}

/// DeletePropertyOrThrow(\p obj, \p index).
ExecutionStatus
deleteIndex(Runtime &runtime, Handle<JSObject> obj, uint32_t index) {
  struct : Locals {
    PinnedValue<> index;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScopeMarkerRAII marker{runtime};
  lv.index = HermesValue::encodeTrustedNumberValue(index);
  return JSObject::deleteComputed(
             obj, runtime, lv.index, PropOpFlags().plusThrowOnError())
      .getStatus();
}

/// Write the \p count sorted \p items back to \p obj, then delete its other
/// indices below \p len, as Array.prototype.sort does.
ExecutionStatus writeBackElements(
    Runtime &runtime,
    Handle<JSObject> obj,
    Handle<JSArray> items,
    uint32_t count,
    uint32_t len) {
  struct : Locals {
    PinnedValue<> index;
    PinnedValue<> value;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  for (uint32_t j = 0; j < count; ++j) {
    GCScopeMarkerRAII marker{runtime};
    lv.index = HermesValue::encodeTrustedNumberValue(j);
    lv.value = items->at(runtime, j).unboxToHV(runtime);
    if (LLVM_UNLIKELY(
            JSObject::putComputed_RJS(
                obj,
                runtime,
                lv.index,
                lv.value,
                PropOpFlags().plusThrowOnError()) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }

  // A proxy or a host object sees every deletion.
  if (obj->isProxyObject(runtime) || obj->isHostObject(runtime)) {
    for (uint32_t k = count; k < len; ++k) {
      if (LLVM_UNLIKELY(
              deleteIndex(runtime, obj, k) == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
    }
    return ExecutionStatus::RETURNED;
  }
  // Deleting a missing property of any other object runs no code, so only
  // its existing indices need deleting.
  std::vector<uint32_t> keys;
  if (LLVM_UNLIKELY(
          appendOwnIndexKeys(runtime, obj, count, len, keys) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  for (uint32_t k : keys) {
    if (LLVM_UNLIKELY(
            deleteIndex(runtime, obj, k) == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }
  return ExecutionStatus::RETURNED;
}

/// Sort \p obj, an object of length \p len, as ES2025 23.1.3.30 does: read the
/// elements into a list, sort the list, write it back, and delete the
/// remaining indices.
ExecutionStatus sortGeneric(
    Runtime &runtime,
    Handle<JSObject> obj,
    Handle<Callable> compareFn,
    uint32_t len) {
  struct : Locals {
    PinnedValue<JSArray> items;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  auto itemsRes = JSArray::create(runtime, 0, 0);
  if (LLVM_UNLIKELY(itemsRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  lv.items = std::move(*itemsRes);
  auto countRes = readElements(runtime, obj, len, lv.items);
  if (LLVM_UNLIKELY(countRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  uint32_t count = *countRes;
  auto newLen = SmallHermesValue::encodeNumberValue(count, runtime);
  JSArray::putLengthUnsafe(lv.items.get(), runtime, newLen);
  if (count > 1) {
    DirectSortModel sm(runtime, lv.items, compareFn);
    if (LLVM_UNLIKELY(quickSort(&sm, 0u, count) == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }
  return writeBackElements(runtime, obj, lv.items, count, len);
}
} // anonymous namespace

/// ES2025 23.1.3.30.
CallResult<HermesValue> arrayPrototypeSort(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // Null if not a callable compareFn.
  auto compareFn = Handle<Callable>::dyn_vmcast(args.getArgHandle(0));
  if (!args.getArg(0).isUndefined() && !compareFn) {
    return runtime.raiseTypeError("Array sort argument must be callable");
  }

  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> lenProp;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*objRes);

  auto propRes = JSObject::getNamed_RJS(
      lv.O, runtime, Predefined::getSymbolID(Predefined::length));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.lenProp = std::move(*propRes);
  auto intRes = toLengthU64(runtime, lv.lenProp);
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t len = *intRes;

  // No sort implementation can handle lengths exceeding UINT32_MAX: quickSort
  // takes uint32_t, and even iterating over that many indices is impractical.
  if (LLVM_UNLIKELY(len > UINT32_MAX))
    return runtime.raiseRangeError("Array sort length exceeds uint32_t");
  auto len32 = static_cast<uint32_t>(len);

  if (vmisa<JSArray>(*lv.O)) {
    auto arr = Handle<JSArray>::vmcast(&lv.O);
    if (arrayFastPathCheck(
            runtime, arr, Runtime::makeNullHandle<HiddenClass>(), len32)) {
      // Writing back fewer than two elements of such an array is
      // unobservable.
      if (len32 > 1 &&
          LLVM_UNLIKELY(
              arrayFastPathSort(runtime, arr, compareFn, len32) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      return lv.O.getHermesValue();
    }
  }

  if (LLVM_UNLIKELY(
          sortGeneric(runtime, lv.O, compareFn, len32) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return lv.O.getHermesValue();
}

/// ES2025 23.1.3.15 Array.prototype.forEach.
inline CallResult<HermesValue> arrayPrototypeForEach(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> O;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // Handles allocated below, for example by getters or native callbacks, are
  // released on every iteration and on return.
  GCScopeMarkerRAII marker{runtime};

  // 1. Let O be ? ToObject(this value).
  // ToObject is the identity on objects, so it is only called for primitives.
  if (LLVM_LIKELY(args.getThisArg().isObject())) {
    lv.O.castAndSetHermesValue<JSObject>(args.getThisArg());
  } else {
    auto objRes = toObject(runtime, args.getThisHandle());
    if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.O.castAndSetHermesValue<JSObject>(*objRes);
  }

  // 2. Let len be ? LengthOfArrayLike(O).
  auto jsArr = vmisa<JSArray>(*lv.O) ? Handle<JSArray>::vmcast(&lv.O)
                                     : Runtime::makeNullHandle<JSArray>();
  auto lenRes = lengthOfArrayLike(runtime, lv.O, jsArr);
  if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  const uint64_t len = *lenRes;

  // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
  auto callbackFn = args.dyncastArg<Callable>(0);
  if (!callbackFn) {
    return runtime.raiseTypeError(
        "Array.prototype.forEach() requires a callable argument");
  }

  // 4. Let k be 0.
  // 5. Repeat, while k < len
  for (uint64_t k = 0; k < len; ++k) {
    marker.flush();

    // 5a. Let Pk be ! ToString(𝔽(k)).
    // 5b. Let kPresent be ? HasProperty(O, Pk).
    // 5c. If kPresent is true, then
    // 5c.i. Let kValue be ? Get(O, Pk).
    // The callback may have modified O, for example shrunk it or added an
    // accessor, so the fast path is checked again on every iteration.
    CallResult<HermesValue> kValueRes = getElementIfPresent(runtime, lv.O, k);
    if (LLVM_UNLIKELY(kValueRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // Skip the index if it is not present.
    if (LLVM_UNLIKELY(kValueRes->isEmpty()))
      continue;

    // 5c.ii. Perform ? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »).
    // kValue is not rooted, but executeCall3() stores it in the new frame
    // before anything can allocate.
    if (LLVM_UNLIKELY(
            Callable::executeCall3(
                callbackFn,
                runtime,
                args.getArgHandle(1),
                *kValueRes,
                HermesValue::encodeTrustedNumberValue(k),
                lv.O.getHermesValue()) == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }

  // 6. Return undefined.
  return HermesValue::encodeUndefinedValue();
}

/// ES10 22.1.3.10.1 FlattenIntoArray
/// mapperFunction may be null to signify its absence.
/// If mapperFunction is null, thisArg is ignored.
static CallResult<uint64_t> flattenIntoArray(
    Runtime &runtime,
    Handle<JSArray> target,
    Handle<JSObject> source,
    uint64_t sourceLen,
    uint64_t start,
    double depth,
    Handle<Callable> mapperFunction,
    Handle<> thisArg) {
  ScopedNativeDepthTracker depthTracker{runtime};
  if (LLVM_UNLIKELY(depthTracker.overflowed())) {
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  }

  if (!mapperFunction) {
    assert(
        thisArg->isUndefined() &&
        "thisArg must be undefined if there is no mapper");
  }

  struct : Locals {
    PinnedValue<> index;
    PinnedValue<SymbolID> tmpPropNameStorage;
    PinnedValue<JSObject> propObj;
    PinnedValue<> element;
    PinnedValue<> len;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  GCScope gcScope{runtime};
  // 1. Let targetIndex be start.
  uint64_t targetIndex = start;
  // 2. Let sourceIndex be 0.
  uint64_t sourceIndex = 0;

  // Temporary storage for sourceIndex and targetIndex.
  MutableHandle<JSObject> propObj{lv.propObj};

  auto marker = gcScope.createMarker();

  // 3. Repeat, while sourceIndex < sourceLen
  while (sourceIndex < sourceLen) {
    gcScope.flushToMarker(marker);

    // a. Let P be ! ToString(sourceIndex).
    // b. Let exists be ? HasProperty(source, P).
    ComputedPropertyDescWithSymStorage desc{lv.tmpPropNameStorage};
    lv.index = HermesValue::encodeTrustedNumberValue(sourceIndex);
    if (LLVM_UNLIKELY(
            JSObject::getComputedDescriptor(
                source, runtime, lv.index, propObj, desc) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // c. If exists is true, then
    // i. Let element be ? Get(source, P).
    CallResult<PseudoHandle<>> elementRes =
        JSObject::getComputedPropertyValue_RJS(
            source, runtime, propObj, desc.get(), lv.index);
    if (LLVM_UNLIKELY(elementRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (LLVM_LIKELY(!(*elementRes)->isEmpty())) {
      lv.element = std::move(*elementRes);

      // ii. If mapperFunction is present, then
      if (mapperFunction) {
        // 1. Assert: thisArg is present.
        assert(!thisArg->isEmpty() && "mapperFunction requires a thisArg");
        // 2. Set element to ? Call(mapperFunction, thisArg , « element,
        // sourceIndex, source »).
        elementRes = Callable::executeCall3(
            mapperFunction,
            runtime,
            thisArg,
            lv.element.getHermesValue(),
            HermesValue::encodeTrustedNumberValue(sourceIndex),
            source.getHermesValue());
        if (LLVM_UNLIKELY(elementRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        lv.element = std::move(*elementRes);
      }
      // iii. Let shouldFlatten be false.
      bool shouldFlatten = false;
      if (depth > 0) {
        // iv. If depth > 0, then
        // 1. Set shouldFlatten to ? IsArray(element).
        // NOTE: isArray accepts nullptr for the obj argument.
        CallResult<bool> shouldFlattenRes =
            isArray(runtime, dyn_vmcast<JSObject>(*lv.element));
        if (LLVM_UNLIKELY(shouldFlattenRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        shouldFlatten = *shouldFlattenRes;
      }
      if (shouldFlatten) {
        // It is valid to cast `element` to JSObject because shouldFlatten is
        // only true when `isArray(element)` is true.
        // v. If shouldFlatten is true, then
        // 1. Let elementLen be ? ToLength(? Get(element, "length")).
        CallResult<PseudoHandle<>> lenRes = JSObject::getNamed_RJS(
            Handle<JSObject>::vmcast(&lv.element),
            runtime,
            Predefined::getSymbolID(Predefined::length));
        if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        lv.len = std::move(*lenRes);
        CallResult<uint64_t> elementLenRes = toLengthU64(runtime, lv.len);
        if (LLVM_UNLIKELY(elementLenRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        uint64_t elementLen = *elementLenRes;
        // 2. Set targetIndex to ? FlattenIntoArray(target, element, elementLen,
        // targetIndex, depth - 1).
        CallResult<uint64_t> targetIndexRes = flattenIntoArray(
            runtime,
            target,
            Handle<JSObject>::vmcast(&lv.element),
            elementLen,
            targetIndex,
            depth - 1,
            Runtime::makeNullHandle<Callable>(),
            runtime.getUndefinedValue());
        if (LLVM_UNLIKELY(targetIndexRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        targetIndex = *targetIndexRes;
      } else {
        // vi. Else,
        // 1. If targetIndex ≥ 2**53-1, throw a TypeError exception.
        if (targetIndex >= ((uint64_t)1 << 53) - 1) {
          return runtime.raiseTypeError("flattened array exceeds length limit");
        }
        // 2. Perform ? CreateDataPropertyOrThrow(
        //                target, !ToString(targetIndex), element).
        lv.index = HermesValue::encodeTrustedNumberValue(targetIndex);
        if (LLVM_UNLIKELY(
                JSObject::defineOwnComputed(
                    target,
                    runtime,
                    lv.index,
                    DefinePropertyFlags::getDefaultNewPropertyFlags(),
                    lv.element,
                    PropOpFlags().plusThrowOnError()) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }

        // 3. Increase targetIndex by 1.
        ++targetIndex;
      }
    }
    // d. Increase sourceIndex by 1.
    ++sourceIndex;
  }
  // 4. Return targetIndex.
  return targetIndex;
}

CallResult<HermesValue> arrayPrototypeFlat(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> len;
    PinnedValue<JSArray> A;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // 1. Let O be ? ToObject(this value).
  CallResult<HermesValue> ORes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(ORes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*ORes);

  // 2. Let sourceLen be ? ToLength(? Get(O, "length")).
  CallResult<PseudoHandle<>> lenRes = JSObject::getNamed_RJS(
      lv.O, runtime, Predefined::getSymbolID(Predefined::length));
  if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.len = std::move(*lenRes);
  CallResult<uint64_t> sourceLenRes = toLengthU64(runtime, lv.len);
  if (LLVM_UNLIKELY(sourceLenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t sourceLen = *sourceLenRes;

  // 3. Let depthNum be 1.
  double depthNum = 1;
  if (!args.getArg(0).isUndefined()) {
    // 4. If depth is not undefined, then
    // a.     Set depthNum to ? ToIntegerOrInfinity(depth).
    auto depthNumRes = toIntegerOrInfinity(runtime, args.getArgHandle(0));
    if (LLVM_UNLIKELY(depthNumRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    depthNum = depthNumRes->getNumber();
  }
  // 5. Let A be ? ArraySpeciesCreate(O, 0).
  auto ARes = JSArray::create(runtime, 0, 0);
  if (LLVM_UNLIKELY(ARes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.A = std::move(*ARes);

  // 6. Perform ? FlattenIntoArray(A, O, sourceLen, 0, depthNum).
  if (LLVM_UNLIKELY(
          flattenIntoArray(
              runtime,
              lv.A,
              lv.O,
              sourceLen,
              0,
              depthNum,
              Runtime::makeNullHandle<Callable>(),
              runtime.getUndefinedValue()) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 7. Return A.
  return lv.A.getHermesValue();
}

CallResult<HermesValue> arrayPrototypeFlatMap(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> len;
    PinnedValue<JSArray> A;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // 1. Let O be ? ToObject(this value).
  CallResult<HermesValue> ORes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(ORes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*ORes);

  // 2. Let sourceLen be ? ToLength(? Get(O, "length")).
  CallResult<PseudoHandle<>> lenRes = JSObject::getNamed_RJS(
      lv.O, runtime, Predefined::getSymbolID(Predefined::length));
  if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.len = std::move(*lenRes);
  CallResult<uint64_t> sourceLenRes = toLengthU64(runtime, lv.len);
  if (LLVM_UNLIKELY(sourceLenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t sourceLen = *sourceLenRes;

  // 3. If IsCallable(mapperFunction) is false, throw a TypeError exception.
  Handle<Callable> mapperFunction = args.dyncastArg<Callable>(0);
  if (!mapperFunction) {
    return runtime.raiseTypeError("flatMap mapper must be callable");
  }
  // 4. If thisArg is present, let T be thisArg; else let T be undefined.
  auto T = args.getArgHandle(1);
  // 5. Let A be ? ArraySpeciesCreate(O, 0).
  auto ARes = JSArray::create(runtime, 0, 0);
  if (LLVM_UNLIKELY(ARes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.A = std::move(*ARes);

  // 6. Perform ? FlattenIntoArray(A, O, sourceLen, 0, 1, mapperFunction, T).
  if (LLVM_UNLIKELY(
          flattenIntoArray(
              runtime, lv.A, lv.O, sourceLen, 0, 1, mapperFunction, T) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 7. Return A.
  return lv.A.getHermesValue();
}

CallResult<HermesValue> arrayPrototypeIterator(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  IterationKind kind = *reinterpret_cast<IterationKind *>(&ctx);
  assert(
      kind < IterationKind::NumKinds &&
      "arrayPrototypeIterator with wrong kind");
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : Locals {
    PinnedValue<JSObject> O;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  lv.O.castAndSetHermesValue<JSObject>(*objRes);
  return JSArrayIterator::create(runtime, lv.O, kind).getHermesValue();
}

CallResult<HermesValue> arrayPrototypeSlice(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> k;
    PinnedValue<JSArray> A;
    PinnedValue<JSObject> descObj;
    PinnedValue<SymbolID> tmpPropNameStorage;
    PinnedValue<> kValue;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  GCScope gcScope(runtime);
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*objRes);

  // 2. Let len be ? LengthOfArrayLike(O).
  // jsArr is O if it is a JSArray, and null otherwise.
  auto jsArr = vmisa<JSArray>(*lv.O) ? Handle<JSArray>::vmcast(&lv.O)
                                     : Runtime::makeNullHandle<JSArray>();
  auto lenRes = lengthOfArrayLike(runtime, lv.O, jsArr);
  if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double len = *lenRes;

  auto intRes = toIntegerOrInfinity(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // Start index. If negative, then offset from the right side of the array.
  double relativeStart = intRes->getNumber();
  // Index that we're currently copying from.
  // Starts at the actual start value, computed from relativeStart.
  double k = relativeStart < 0 ? std::max(len + relativeStart, 0.0)
                               : std::min(relativeStart, len);

  // End index. If negative, then offset from the right side of the array.
  double relativeEnd;
  if (args.getArg(1).isUndefined()) {
    relativeEnd = len;
  } else {
    if (LLVM_UNLIKELY(
            (intRes = toIntegerOrInfinity(runtime, args.getArgHandle(1))) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    relativeEnd = intRes->getNumber();
  }
  // Actual end index.
  double fin = relativeEnd < 0 ? std::max(len + relativeEnd, 0.0)
                               : std::min(relativeEnd, len);

  // Create the result array.
  double count = std::max(fin - k, 0.0);
  uint32_t count32 = truncateToUInt32(count);
  if (LLVM_UNLIKELY(count32 != count)) {
    return runtime.raiseRangeError("invalid array length");
  }

  // Fast path for arrays that pass arrayFastPathCheck(): all elements of O
  // are plain data properties stored at [0, len) of its indexed storage, and
  // no object on its prototype chain has index-like properties. Then reading
  // an element cannot run user code and a hole reads as absent, so [k, fin)
  // can be copied directly from the storage, holes included. This must be
  // checked after the ToIntegerOrInfinity() calls above, since they may run
  // user code that modifies O. The copied values may share BoxedDoubles with
  // O, which is fine because BoxedDoubles are immutable.
  // Like the slow path, this does not implement ArraySpeciesCreate. If that
  // is ever implemented, this path must also check that it would create a
  // plain Array.
  if (jsArr &&
      arrayFastPathCheck(
          runtime,
          jsArr,
          Runtime::makeNullHandle<HiddenClass>(),
          static_cast<uint32_t>(len))) {
    auto arrRes = JSArray::create(runtime, count32, 0);
    if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.A = std::move(*arrRes);
    // An empty result has no storage to copy into.
    if (count32 == 0)
      return lv.A.getHermesValue();
    // A is freshly created with the default class, so length is writable.
    // May allocate, so encode before NoAllocScope.
    auto newLen = SmallHermesValue::encodeNumberValue(count32, runtime);
    NoAllocScope noAlloc{runtime};
    // Read the storages only now, since the allocations above may move them.
    auto *srcStorage = jsArr->getIndexedStorageNullable(runtime);
    auto *destStorage = lv.A->getIndexedStorageNullable(runtime);
    assert(srcStorage && destStorage && "storage must not be null");
    uint32_t from = static_cast<uint32_t>(k);
    destStorage->appendWithinCapacity(
        runtime, srcStorage, from, from + count32);
    lv.A->setElemCountUnsafe(count32);
    JSArray::putLengthUnsafe(lv.A.get(), runtime, newLen);
    return lv.A.getHermesValue();
  }

  auto arrRes = JSArray::create(runtime, count, count);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.A = std::move(*arrRes);

  // Next index in A to write to.
  uint32_t n = 0;

  auto marker = gcScope.createMarker();

  // Copy the elements between the actual start and end indices into A.
  while (k < fin) {
    lv.k = HermesValue::encodeTrustedNumberValue(k);
    ComputedPropertyDescWithSymStorage desc{lv.tmpPropNameStorage};
    JSObject::getComputedPrimitiveDescriptor(
        lv.O, runtime, lv.k, lv.descObj, desc);
    CallResult<PseudoHandle<>> propRes = JSObject::getComputedPropertyValue_RJS(
        lv.O, runtime, lv.descObj, desc.get(), lv.k);
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (LLVM_LIKELY(!(*propRes)->isEmpty())) {
      lv.kValue = std::move(*propRes);
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(lv.A, runtime, n, lv.kValue) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
    }
    k += 1;
    ++n;

    gcScope.flushToMarker(marker);
  }

  if (LLVM_UNLIKELY(
          JSArray::setLengthProperty(lv.A, runtime, n) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return lv.A.getHermesValue();
}

/// Fast path for Array.prototype.slice() when the array is a normal array.
/// \pre \p O is an array with fast index properties and there are no index-like
///   properties in any parents.
/// \pre \p The storage for \p O starts at 0 and ends at its length.
/// \param O the array to splice.
/// \param len the length of O.
/// \param A the array to populate with deleted elements.
/// \param actualStart the start index to splice.
/// \param actualDeleteCount the number of elements to delete.
/// \param itemCount the number of elements to insert from \p args.
/// \param args the original NativeArgs to splice().
/// \return the result of the splice (\p A).
static CallResult<HermesValue> arrayPrototypeSpliceFastPath(
    Runtime &runtime,
    Handle<JSArray> O,
    uint32_t len,
    Handle<JSArray> A,
    uint32_t actualStart,
    uint32_t actualDeleteCount,
    uint32_t itemCount,
    NativeArgs args) {
  assert(
      O->hasFastIndexProperties(runtime) &&
      "O must have fast index properties");
  assert(!O->isProxyObject(runtime) && "O must not be proxy");
  assert(O->getBeginIndex() == 0 && "incorrect begin index");
  assert(O->getEndIndex() == len && "incorrect end index");

  // Populate A (the result array) with the deleted elements.
  // No need to set the length property, since it was already set when A was
  // allocated.
  if (LLVM_UNLIKELY(
          JSArray::setStorageEndIndex(A, runtime, actualDeleteCount) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  {
    NoAllocScope noAlloc(runtime);
    JSArray::StorageType *aStorage = A->getIndexedStorageNullable(runtime);
    JSArray::StorageType *oStorage = O->getIndexedStorageNullable(runtime);
    for (uint32_t j = 0; j < actualDeleteCount; ++j) {
      assert(aStorage && oStorage && "storage must not be null");
      uint32_t from = actualStart + j;
      SmallHermesValue elem = oStorage->at(from);
      aStorage->set(j, elem, runtime.getHeap());
    }
  }

  // Final length of O.
  assert(actualDeleteCount <= len && "actualDeleteCount out of range");
  assert(
      UINT32_MAX - itemCount > len - actualDeleteCount &&
      "overflow must be checked before calling fast path");
  uint32_t finalLen = len - actualDeleteCount + itemCount;

  if (itemCount < actualDeleteCount) {
    // Inserting less items than deleting.

    // Shift non-deleted items to the left.
    // Copy items from (k + actualDeleteCount) to (k + itemCount).
    // This leaves itemCount spaces to copy the arguments into.

    NoAllocScope noAlloc(runtime);
    JSArray::StorageType *oStorage = O->getIndexedStorageNullable(runtime);
    for (uint32_t k = actualStart; k < len - actualDeleteCount; ++k) {
      assert(oStorage && "storage must not be null");
      uint32_t from = k + actualDeleteCount;
      uint32_t to = k + itemCount;
      SmallHermesValue elem = oStorage->at(from);
      oStorage->set(to, elem, runtime.getHeap());
    }

    // Shrink the array size to the new length.
    JSArray::StorageType::resizeWithinCapacity(oStorage, runtime, finalLen);
  } else if (itemCount > actualDeleteCount) {
    // Inserting more items than deleting.

    // Expand the array to make room for the new items.
    // Length property will be set at the end.
    if (LLVM_UNLIKELY(
            JSArray::setStorageEndIndex(O, runtime, finalLen) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // Start from the right, and copy elements to the right.
    // This makes space to insert the elements from the arguments.
    // Loop k from (len - actualDeleteCount) to actualStart (exclusive),
    // just like the spec does for the slow path.

    NoAllocScope noAlloc(runtime);
    JSArray::StorageType *oStorage = O->getIndexedStorageNullable(runtime);
    for (uint32_t k = len - actualDeleteCount; k > actualStart; --k) {
      assert(oStorage && "storage must not be null");
      uint32_t from = k + actualDeleteCount - 1;
      uint32_t to = k + itemCount - 1;
      SmallHermesValue elem = oStorage->at(from);
      oStorage->set(to, elem, runtime.getHeap());
    }
  }

  // Finally, just copy the elements from the args into the array
  // where they belong.
  // NOTE: This may allocate because we're converting to SmallHermesValue,
  // so we can't keep the storage around across iterations.
  for (uint32_t j = 0; j < itemCount; ++j) {
    // Declare a local variable so that SmallHermesValue::encodeHermesValue
    // (possible allocation) happens before *O.
    // Avoids argument reordering problems.
    // Get args starting at 2 because the first two args are start and
    // deleteCount.
    SmallHermesValue elem =
        SmallHermesValue::encodeHermesValue(args.getArg(j + 2), runtime);
    JSArray::unsafeSetExistingElementAt(*O, runtime, actualStart + j, elem);
  }

  if (LLVM_UNLIKELY(
          JSArray::setLengthProperty(
              O, runtime, finalLen, PropOpFlags().plusThrowOnError()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return A.getHermesValue();
}

CallResult<HermesValue> arrayPrototypeSplice(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<JSArray> OArray;
    PinnedValue<> lenProp;
    PinnedValue<JSArray> A;
    PinnedValue<> from;
    PinnedValue<> to;
    PinnedValue<JSObject> fromDescObj;
    PinnedValue<SymbolID> tmpPropNameStorage;
    PinnedValue<> fromValue;
    PinnedValue<> i;
    PinnedValue<> k;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  GCScope gcScope(runtime);

  uint64_t len;
  if (LLVM_LIKELY(vmisa<JSArray>(args.getThisArg()))) {
    lv.O.castAndSetHermesValue<JSObject>(args.getThisArg());
    lv.OArray.castAndSetHermesValue<JSArray>(args.getThisArg());
    len = JSArray::getLength(vmcast<JSArray>(args.getThisArg()), runtime);
  } else {
    auto objRes = toObject(runtime, args.getThisHandle());
    if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.O.castAndSetHermesValue<JSObject>(*objRes);
    auto propRes = JSObject::getNamed_RJS(
        lv.O, runtime, Predefined::getSymbolID(Predefined::length));
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.lenProp = std::move(*propRes);
    auto lenRes = toLengthU64(runtime, lv.lenProp);
    if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    len = *lenRes;
  }

  auto intRes = toIntegerOrInfinity(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // Must use double here to account for Infinity.
  double relativeStart = intRes->getNumber();
  // Index to start the deletion/insertion at.
  double actualStart = relativeStart < 0 ? std::max(len + relativeStart, 0.0)
                                         : std::min(relativeStart, (double)len);

  // Implement the newer calculation of actualDeleteCount (ES6.0),
  // since 5.1 doesn't define behavior for less than 2 arguments.
  uint32_t argCount = args.getArgCount();
  uint64_t actualDeleteCount;
  uint64_t insertCount;
  switch (argCount) {
    case 0:
      insertCount = 0;
      actualDeleteCount = 0;
      break;
    case 1:
      // If just one argument specified, delete everything until the end.
      insertCount = 0;
      actualDeleteCount = len - actualStart;
      break;
    default:
      // Otherwise, use the specified delete count.
      if (LLVM_UNLIKELY(
              (intRes = toIntegerOrInfinity(runtime, args.getArgHandle(1))) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      insertCount = argCount - 2;
      actualDeleteCount =
          std::min(std::max(intRes->getNumber(), 0.0), len - actualStart);
  }

  // If len+insertCount−actualDeleteCount > 2^53-1, throw a TypeError exception.
  // Checks for overflow as well.
  uint64_t lenAfterInsert = len + insertCount;
  if (LLVM_UNLIKELY(
          lenAfterInsert < len ||
          lenAfterInsert - actualDeleteCount > (1LLU << 53) - 1)) {
    return runtime.raiseTypeError("Array.prototype.splice result out of space");
  }

  if (LLVM_UNLIKELY(actualDeleteCount > std::numeric_limits<uint32_t>::max())) {
    return runtime.raiseRangeError("invalid array length");
  }
  auto arrRes = JSArray::create(runtime, actualDeleteCount, actualDeleteCount);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.A = std::move(*arrRes);

  // Number of new items to add to the array.
  uint32_t itemCount = args.getArgCount() > 2 ? args.getArgCount() - 2 : 0;

  // If we can use the fast path, do so.
  if (LLVM_LIKELY(*lv.OArray) &&
      LLVM_LIKELY(
          arrayFastPathCheck(runtime, lv.OArray, runtime.classJSArray, len)) &&
      LLVM_LIKELY(len - actualDeleteCount < UINT32_MAX - itemCount)) {
    // These can be cast to uint32_t because we know that O is a JSArray.
    assert(actualStart <= len && "actualStart out of range");
    assert(actualDeleteCount <= len && "actualDeleteCount out of range");
    return arrayPrototypeSpliceFastPath(
        runtime,
        lv.OArray,
        (uint32_t)len,
        lv.A,
        (uint32_t)actualStart,
        (uint32_t)actualDeleteCount,
        itemCount,
        args);
  }

  auto gcMarker = gcScope.createMarker();
  {
    // Copy actualDeleteCount elements to A, starting at actualStart.
    for (uint32_t j = 0; j < actualDeleteCount; ++j) {
      lv.from = HermesValue::encodeTrustedNumberValue(actualStart + j);

      ComputedPropertyDescWithSymStorage fromDesc{lv.tmpPropNameStorage};
      JSObject::getComputedPrimitiveDescriptor(
          lv.O, runtime, lv.from, lv.fromDescObj, fromDesc);
      CallResult<PseudoHandle<>> propRes =
          JSObject::getComputedPropertyValue_RJS(
              lv.O, runtime, lv.fromDescObj, fromDesc.get(), lv.from);
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (LLVM_LIKELY(!(*propRes)->isEmpty())) {
        lv.fromValue = std::move(*propRes);
        if (LLVM_UNLIKELY(
                JSArray::setElementAt(lv.A, runtime, j, lv.fromValue) ==
                ExecutionStatus::EXCEPTION))
          return ExecutionStatus::EXCEPTION;
      }

      gcScope.flushToMarker(gcMarker);
    }

    if (LLVM_UNLIKELY(
            JSArray::setLengthProperty(lv.A, runtime, actualDeleteCount) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }

  lv.lenProp = HermesValue::encodeTrustedNumberValue(actualDeleteCount);
  // Perform ? Set(A, "length", actualDeleteCount, true).
  if (LLVM_UNLIKELY(
          JSObject::putNamed_RJS(
              lv.A,
              runtime,
              Predefined::getSymbolID(Predefined::length),
              lv.lenProp,
              PropOpFlags().plusThrowOnError()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (itemCount < actualDeleteCount) {
    // Inserting less items than deleting.

    // Copy items from (k + actualDeleteCount) to (k + itemCount).
    // This leaves itemCount spaces to copy the arguments into.
    for (double j = actualStart; j < len - actualDeleteCount; ++j) {
      lv.from = HermesValue::encodeTrustedNumberValue(j + actualDeleteCount);
      lv.to = HermesValue::encodeTrustedNumberValue(j + itemCount);
      ComputedPropertyDescWithSymStorage fromDesc{lv.tmpPropNameStorage};
      JSObject::getComputedPrimitiveDescriptor(
          lv.O, runtime, lv.from, lv.fromDescObj, fromDesc);
      CallResult<PseudoHandle<>> propRes =
          JSObject::getComputedPropertyValue_RJS(
              lv.O, runtime, lv.fromDescObj, fromDesc.get(), lv.from);
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (LLVM_LIKELY(!(*propRes)->isEmpty())) {
        lv.fromValue = std::move(*propRes);
        if (LLVM_UNLIKELY(
                JSObject::putComputed_RJS(
                    lv.O,
                    runtime,
                    lv.to,
                    lv.fromValue,
                    PropOpFlags().plusThrowOnError()) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
      } else {
        if (LLVM_UNLIKELY(
                JSObject::deleteComputed(
                    lv.O, runtime, lv.to, PropOpFlags().plusThrowOnError()) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
      }

      gcScope.flushToMarker(gcMarker);
    }

    // Use i here to refer to (k-1) in the spec, and reindex the loop.
    double i = len - 1;

    // Delete the remaining elements from the right that we didn't copy into.
    while (i > (len - actualDeleteCount + itemCount - 1)) {
      lv.i = HermesValue::encodeTrustedNumberValue(i);
      if (LLVM_UNLIKELY(
              JSObject::deleteComputed(
                  lv.O, runtime, lv.i, PropOpFlags().plusThrowOnError()) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      i -= 1;
      gcScope.flushToMarker(gcMarker);
    }
  } else if (itemCount > actualDeleteCount) {
    // Inserting more items than deleting.

    // Start from the right, and copy elements to the right.
    // This makes space to insert the elements from the arguments.
    for (double j = len - actualDeleteCount; j > actualStart; --j) {
      lv.from =
          HermesValue::encodeTrustedNumberValue(j + actualDeleteCount - 1);
      lv.to = HermesValue::encodeTrustedNumberValue(j + itemCount - 1);

      ComputedPropertyDescWithSymStorage fromDesc{lv.tmpPropNameStorage};
      JSObject::getComputedPrimitiveDescriptor(
          lv.O, runtime, lv.from, lv.fromDescObj, fromDesc);
      CallResult<PseudoHandle<>> propRes =
          JSObject::getComputedPropertyValue_RJS(
              lv.O, runtime, lv.fromDescObj, fromDesc.get(), lv.from);
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (LLVM_LIKELY(!(*propRes)->isEmpty())) {
        lv.fromValue = std::move(*propRes);
        if (LLVM_UNLIKELY(
                JSObject::putComputed_RJS(
                    lv.O,
                    runtime,
                    lv.to,
                    lv.fromValue,
                    PropOpFlags().plusThrowOnError()) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
      } else {
        // fromPresent is false
        if (LLVM_UNLIKELY(
                JSObject::deleteComputed(
                    lv.O, runtime, lv.to, PropOpFlags().plusThrowOnError()) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
      }

      gcScope.flushToMarker(gcMarker);
    }
  }

  {
    // Finally, just copy the elements from the args into the array.
    lv.k = HermesValue::encodeTrustedNumberValue(actualStart);
    for (size_t j = 2; j < argCount; ++j) {
      if (LLVM_UNLIKELY(
              JSObject::putComputed_RJS(
                  lv.O,
                  runtime,
                  lv.k,
                  args.getArgHandle(j),
                  PropOpFlags().plusThrowOnError()) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.k = HermesValue::encodeTrustedNumberValue(lv.k->getDouble() + 1);
      gcScope.flushToMarker(gcMarker);
    }
  }

  lv.lenProp = HermesValue::encodeTrustedNumberValue(
      len - actualDeleteCount + itemCount);
  if (LLVM_UNLIKELY(
          JSObject::putNamed_RJS(
              lv.O,
              runtime,
              Predefined::getSymbolID(Predefined::length),
              lv.lenProp,
              PropOpFlags().plusThrowOnError()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return lv.A.getHermesValue();
}

CallResult<HermesValue> arrayPrototypeCopyWithin(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> lenProp;
    PinnedValue<> from;
    PinnedValue<> to;
    PinnedValue<> fromVal;
    PinnedValue<JSObject> fromObj;
    PinnedValue<SymbolID> fromNameTmpStorage;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScope gcScope{runtime};

  // 1. Let O be ToObject(this value).
  // 2. ReturnIfAbrupt(O).
  auto oRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(oRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*oRes);

  // 3. Let len be ToLength(Get(O, "length")).
  // 4. ReturnIfAbrupt(len).
  // Use doubles for all lengths and indices to allow for proper Infinity
  // handling, because ToInteger may return Infinity and we must do double
  // arithmetic.
  auto propRes = JSObject::getNamed_RJS(
      lv.O, runtime, Predefined::getSymbolID(Predefined::length));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.lenProp = std::move(*propRes);
  auto lenRes = toLengthU64(runtime, lv.lenProp);
  if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double len = *lenRes;

  // 5. Let relativeTarget be ToIntegerOrInfinity(target).
  // 6. ReturnIfAbrupt(relativeTarget).
  auto relativeTargetRes = toIntegerOrInfinity(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(relativeTargetRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double relativeTarget = relativeTargetRes->getNumber();

  // 7. If relativeTarget < 0, let to be max((len + relativeTarget),0); else let
  // to be min(relativeTarget, len).
  double to = relativeTarget < 0 ? std::max((len + relativeTarget), (double)0)
                                 : std::min(relativeTarget, len);

  // 8. Let relativeStart be ToIntegerOrInfinity(start).
  // 9. ReturnIfAbrupt(relativeStart).
  auto relativeStartRes = toIntegerOrInfinity(runtime, args.getArgHandle(1));
  if (LLVM_UNLIKELY(relativeStartRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double relativeStart = relativeStartRes->getNumber();

  // 10. If relativeStart < 0, let from be max((len + relativeStart),0); else
  // let from be min(relativeStart, len).
  double from = relativeStart < 0 ? std::max((len + relativeStart), (double)0)
                                  : std::min(relativeStart, len);

  // 11. If end is undefined, let relativeEnd be len; else let relativeEnd be
  // ToIntegerOrInfinity(end).
  // 12. ReturnIfAbrupt(relativeEnd).
  double relativeEnd;
  if (args.getArg(2).isUndefined()) {
    relativeEnd = len;
  } else {
    auto relativeEndRes = toIntegerOrInfinity(runtime, args.getArgHandle(2));
    if (LLVM_UNLIKELY(relativeEndRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    relativeEnd = relativeEndRes->getNumber();
  }

  // 13. If relativeEnd < 0, let final be max((len + relativeEnd),0); else let
  // final be min(relativeEnd, len).
  double fin = relativeEnd < 0 ? std::max((len + relativeEnd), (double)0)
                               : std::min(relativeEnd, len);

  // 14. Let count be min(final-from, len-to).
  double count = std::min(fin - from, len - to);

  int direction;
  if (from < to && to < from + count) {
    // 15. If from<to and to<from+count
    // a. Let direction be -1.
    direction = -1;
    // b. Let from be from + count -1.
    from = from + count - 1;
    // c. Let to be to + count -1.
    to = to + count - 1;
  } else {
    // 16. Else,
    // a. Let direction = 1.
    direction = 1;
  }

  lv.from = HermesValue::encodeTrustedNumberValue(from);
  lv.to = HermesValue::encodeTrustedNumberValue(to);

  GCScopeMarkerRAII marker{gcScope};
  for (; count > 0; marker.flush()) {
    // 17. Repeat, while count > 0
    // a. Let fromKey be ToString(from).
    // b. Let toKey be ToString(to).

    // c. Let fromPresent be HasProperty(O, fromKey).
    // d. ReturnIfAbrupt(fromPresent).
    ComputedPropertyDescWithSymStorage fromDesc{lv.fromNameTmpStorage};
    if (LLVM_UNLIKELY(
            JSObject::getComputedDescriptor(
                lv.O, runtime, lv.from, lv.fromObj, fromDesc) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    CallResult<PseudoHandle<>> fromValRes =
        JSObject::getComputedPropertyValue_RJS(
            lv.O, runtime, lv.fromObj, fromDesc.get(), lv.from);
    if (LLVM_UNLIKELY(fromValRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // e. If fromPresent is true, then
    if (LLVM_LIKELY(!(*fromValRes)->isEmpty())) {
      // i. Let fromVal be Get(O, fromKey).
      // ii. ReturnIfAbrupt(fromVal).
      lv.fromVal = std::move(*fromValRes);

      // iii. Let setStatus be Set(O, toKey, fromVal, true).
      // iv. ReturnIfAbrupt(setStatus).
      if (LLVM_UNLIKELY(
              JSObject::putComputed_RJS(
                  lv.O,
                  runtime,
                  lv.to,
                  lv.fromVal,
                  PropOpFlags().plusThrowOnError()) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    } else {
      // f. Else fromPresent is false,
      // i. Let deleteStatus be DeletePropertyOrThrow(O, toKey).
      // ii. ReturnIfAbrupt(deleteStatus).
      if (LLVM_UNLIKELY(
              JSObject::deleteComputed(
                  lv.O, runtime, lv.to, PropOpFlags().plusThrowOnError()) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    }

    // g. Let from be from + direction.
    lv.from =
        HermesValue::encodeTrustedNumberValue(lv.from->getNumber() + direction);
    // h. Let to be to + direction.
    lv.to =
        HermesValue::encodeTrustedNumberValue(lv.to->getNumber() + direction);

    // i. Let count be count − 1.
    --count;
  }
  // 18. Return O.
  return lv.O.getHermesValue();
}

static CallResult<HermesValue>
arrayPrototypePopFastPath(Runtime &runtime, Handle<JSArray> arr, uint32_t len) {
  if (LLVM_UNLIKELY(len == 0)) {
    return HermesValue::encodeUndefinedValue();
  }

  // May allocate, do the encoding outside NoAllocScope.
  auto newLen = SmallHermesValue::encodeNumberValue(len - 1, runtime);

  // Perform the actual pop.
  NoAllocScope noAlloc{runtime};
  auto *storage = arr->getIndexedStorageUnsafe(runtime);
  SmallHermesValue shv = storage->pop_back(runtime);
  // Set the elemCount to the end of the storage, which we know is correct
  // because we just popped to len-1 elements and we've already checked the
  // bounds of the storage in the fast path check.
  assert(storage->size() == len - 1 && arr->getBeginIndex() == 0);
  arr->setElemCountUnsafe(len - 1);
  // We've already checked that the length is not readonly.
  JSArray::putLengthUnsafe(*arr, runtime, newLen);

  // Fast path check has ensured there's no other elements up the prototype
  // chain that can have values at index-like property names, so if we see
  // 'empty' in the storage we need to return 'undefined'.
  return shv.isEmpty() ? HermesValue::encodeUndefinedValue()
                       : shv.unboxToHV(runtime);
}

CallResult<HermesValue> arrayPrototypePop(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> lenProp;
    PinnedValue<> element;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // Ensure the fast path does not leak any handles.
  NoLeakHandleScope noLeaks{runtime};

  uint64_t len;
  if (LLVM_LIKELY(vmisa<JSArray>(args.getThisArg()))) {
    // Fast path for getting the length.
    auto arr = args.vmcastThis<JSArray>();
    len = JSArray::getLength(*arr, runtime);

    if (arrayFastPathCheck(runtime, arr, runtime.classJSArray, len)) {
      return arrayPrototypePopFastPath(
          runtime, args.vmcastThis<JSArray>(), len);
    }

    // Fast path check failed, populate the O Local so it can be used in the
    // slow path.
    lv.O = args.vmcastThis<JSObject>();
  }

  // The slow path may create additional handles, so create a GCScope to avoid
  // leaking them.
  GCScope gcScope(runtime);

  // If the fast path has not populated the object, do that now.
  if (!*lv.O) {
    auto res = toObject(runtime, args.getThisHandle());
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.O.castAndSetHermesValue<JSObject>(*res);
    auto propRes = JSObject::getNamed_RJS(
        lv.O, runtime, Predefined::getSymbolID(Predefined::length));
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.lenProp = std::move(*propRes);
    auto intRes = toLengthU64(runtime, lv.lenProp);
    if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    len = *intRes;
  }

  if (len == 0) {
    lv.lenProp = HermesValue::encodeTrustedNumberValue(0);
    if (LLVM_UNLIKELY(
            JSObject::putNamed_RJS(
                lv.O,
                runtime,
                Predefined::getSymbolID(Predefined::length),
                lv.lenProp,
                PropOpFlags().plusThrowOnError()) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    return HermesValue::encodeUndefinedValue();
  }

  lv.lenProp = HermesValue::encodeTrustedNumberValue(len - 1);
  CallResult<PseudoHandle<>> propRes = getIndexed_RJS(runtime, lv.O, len - 1);
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.element = std::move(*propRes);
  if (LLVM_UNLIKELY(
          JSObject::deleteComputed(
              lv.O, runtime, lv.lenProp, PropOpFlags().plusThrowOnError()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (LLVM_UNLIKELY(
          JSObject::putNamed_RJS(
              lv.O,
              runtime,
              Predefined::getSymbolID(Predefined::length),
              lv.lenProp,
              PropOpFlags().plusThrowOnError()) == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return lv.element.get();
}

CallResult<HermesValue> arrayPrototypeShift(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> lenProp;
    PinnedValue<> from;
    PinnedValue<> to;
    PinnedValue<> fromVal;
    PinnedValue<SymbolID> fromNameTmpStorage;
    PinnedValue<JSObject> fromDescObj;
    PinnedValue<> first;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  GCScope gcScope(runtime);
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*objRes);

  auto propRes = JSObject::getNamed_RJS(
      lv.O, runtime, Predefined::getSymbolID(Predefined::length));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.lenProp = std::move(*propRes);
  auto intRes = toLengthU64(runtime, lv.lenProp);
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t len = *intRes;

  if (len == 0) {
    // Need to set length to 0 per spec.
    lv.lenProp = HermesValue::encodeTrustedNumberValue(0);
    if (JSObject::putNamed_RJS(
            lv.O,
            runtime,
            Predefined::getSymbolID(Predefined::length),
            lv.lenProp,
            PropOpFlags().plusThrowOnError()) == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    return HermesValue::encodeUndefinedValue();
  }

  if (LLVM_UNLIKELY(
          (propRes = getIndexed_RJS(runtime, lv.O, 0)) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.first = std::move(*propRes);

  lv.from = HermesValue::encodeTrustedNumberValue(1);

  // Move every element to the left one slot.
  // TODO: Add a fast path for actual arrays.
  while (lv.from->getDouble() < len) {
    GCScopeMarkerRAII marker{gcScope};

    // Moving an element from "from" to "from - 1".
    lv.to = HermesValue::encodeTrustedNumberValue(lv.from->getDouble() - 1);

    ComputedPropertyDescWithSymStorage fromDesc{lv.fromNameTmpStorage};
    JSObject::getComputedPrimitiveDescriptor(
        lv.O, runtime, lv.from, lv.fromDescObj, fromDesc);
    CallResult<PseudoHandle<>> propRes = JSObject::getComputedPropertyValue_RJS(
        lv.O, runtime, lv.fromDescObj, fromDesc.get(), lv.from);
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    if (LLVM_LIKELY(!(*propRes)->isEmpty())) {
      // fromPresent is true, so read fromVal and set the "to" index.
      lv.fromVal = std::move(*propRes);
      if (LLVM_UNLIKELY(
              JSObject::putComputed_RJS(
                  lv.O,
                  runtime,
                  lv.to,
                  lv.fromVal,
                  PropOpFlags().plusThrowOnError()) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    } else {
      // fromVal is not present so move the empty slot to the left.
      if (LLVM_UNLIKELY(
              JSObject::deleteComputed(
                  lv.O, runtime, lv.to, PropOpFlags().plusThrowOnError()) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    }

    lv.from = HermesValue::encodeTrustedNumberValue(lv.from->getDouble() + 1);
  }

  lv.lenProp = HermesValue::encodeTrustedNumberValue(len - 1);
  // Delete last element of the array.
  if (LLVM_UNLIKELY(
          JSObject::deleteComputed(
              lv.O, runtime, lv.lenProp, PropOpFlags().plusThrowOnError()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // Decrement length.
  if (LLVM_UNLIKELY(
          JSObject::putNamed_RJS(
              lv.O,
              runtime,
              Predefined::getSymbolID(Predefined::length),
              lv.lenProp,
              PropOpFlags().plusThrowOnError()) == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return lv.first.get();
}

/// SIMD-accelerated linear search for a raw value in an array of
/// SmallHermesValue elements.
/// \p reverse when true, search backward returning the last match;
///   when false, search forward returning the first match.
/// \return the index of the match, or -1 if not found.
template <typename T>
static int64_t fastSearch(
    llvh::ArrayRef<T> data,
    size_t start,
    size_t end,
    T target,
    bool reverse) {
  if constexpr (sizeof(T) == 4) {
    return reverse ? searchReverseU32(data, start, end, target)
                   : searchU32(data, start, end, target);
  } else {
    static_assert(sizeof(T) == 8, "unexpected element size");
    return reverse ? searchReverseU64(data, start, end, target)
                   : searchU64(data, start, end, target);
  }
}

/// Used to help with indexOf and lastIndexOf.
/// \p reverse true if searching in reverse (lastIndexOf), false otherwise.
static inline CallResult<HermesValue>
indexOfHelper(Runtime &runtime, NativeArgs args, const bool reverse) {
  GCScope gcScope(runtime);
  struct : Locals {
    PinnedValue<JSObject> obj;
    PinnedValue<> lenProp;
    PinnedValue<SymbolID> tmpPropNameStorage;
    PinnedValue<JSObject> descObj;
    PinnedValue<> k;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.obj.castAndSetHermesValue<JSObject>(*objRes);

  // Array length is less than 2^32, length of array-like object is less than
  // 2^53.
  uint64_t len;
  auto arrHandle = vmisa<JSArray>(*lv.obj) ? Handle<JSArray>::vmcast(&lv.obj)
                                           : Runtime::makeNullHandle<JSArray>();
  if (LLVM_LIKELY(arrHandle)) {
    // Fast path: get array length.
    len = JSArray::getLength(arrHandle.get(), runtime);
  } else {
    auto propRes = JSObject::getNamed_RJS(
        lv.obj, runtime, Predefined::getSymbolID(Predefined::length));
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.lenProp = std::move(*propRes);
    auto lenRes = toLengthU64(runtime, lv.lenProp);
    if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // toLengthU64() returns an unsigned integer smaller than 2^53, so it fits
    // into int64_t.
    len = *lenRes;
  }

  // Early return before running into any coercions on args.
  // 2. Let len be ? LengthOfArrayLike(O).
  // 3. If len is 0, return -1.
  if (len == 0) {
    return HermesValue::encodeTrustedNumberValue(-1);
  }

  // Actual index to start the search at.
  uint64_t k;
  if (args.getArgCount() > 1) {
    auto intRes = toIntegerOrInfinity(runtime, args.getArgHandle(1));
    if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    double n = intRes->getNumber();
    if (!reverse) {
      if (LLVM_UNLIKELY(n >= len)) {
        // If n >= len, nothing to search. This also handles n = +Inf.
        return HermesValue::encodeTrustedNumberValue(-1);
      } else if (n < 0) {
        // 9. k = len + n. If k < 0, set k = 0. This handles n == -Inf.
        k = std::max(len + n, 0.0);
      } else {
        // Here 0 <= n < len.
        k = n;
      }
    } else {
      if (LLVM_UNLIKELY(n + len < 0)) {
        // Return -1 since nothing to search. This handles n = -Inf.
        return HermesValue::encodeTrustedNumberValue(-1);
      } else if (n >= 0) {
        // If n is larger than len - 1, set k = len - 1. This handles n = +Inf.
        k = std::min(n, (double)(len - 1));
      } else {
        // Here 0 <= n + len < len.
        k = len + n;
      }
    }
  } else {
    k = !reverse ? 0 : len - 1;
  }

  // Search for the element.
  auto searchElement = args.getArgHandle(0);
  // Fast path.
  if (LLVM_LIKELY(
          arrHandle &&
          arrayFastPathCheck(
              runtime,
              arrHandle,
              Runtime::makeNullHandle<HiddenClass>(),
              (uint32_t)len))) {
    // SmallHermesValue::getRaw() may return uintptr_t, which can be a
    // different type from uint32_t/uint64_t on some platforms (e.g.
    // unsigned long vs unsigned long long).
    using SearchType = std::conditional_t<
        sizeof(SmallHermesValue::RawType) == 4,
        uint32_t,
        uint64_t>;
    auto searchElementVal =
        SmallHermesValue::encodeHermesValue(searchElement.get(), runtime);
    NoAllocScope noAlloc{runtime};
    assert(len != 0 && "we already checked len != 0, so storage can't be null");
    auto *arrStorage = arrHandle->getIndexedStorageUnsafe(runtime);

    llvh::ArrayRef<SearchType> rawData(
        reinterpret_cast<const SearchType *>(arrStorage->data()), len);
    auto rawTarget = static_cast<SearchType>(searchElementVal.getRaw());

    // Macro for searching the array with given loop initialization, terminating
    // condition and updating rule after each iteration. Note that the macro
    // COMPARE_EXPR, which takes the array element of each iteration and
    // produces a boolean value, must be defined befofe exeucting this.
#define SEARCH_ARRAY_DIRECTED(INIT, LOOP_COND, STEP)   \
  for (INIT; LOOP_COND; STEP) {                        \
    auto element = arrStorage->at(k);                  \
    if (COMPARE_EXPR(element))                         \
      return HermesValue::encodeTrustedNumberValue(k); \
  }

    // Specialize the loop with search direction.
#define SEARCH_ARRAY                             \
  if (reverse) {                                 \
    SEARCH_ARRAY_DIRECTED(++k, k-- > 0, (void)0) \
  } else {                                       \
    SEARCH_ARRAY_DIRECTED((void)0, k < len, ++k) \
  }

    // Specialize the search loop with the type of the target value.
    if (searchElementVal.isInlinedDouble()) {
      auto searchNum = searchElementVal.getNumber(runtime);
      // If it's NaN, no need to do any comparison.
      if (LLVM_UNLIKELY(std::isnan(searchNum))) {
        return HermesValue::encodeTrustedNumberValue(-1);
      }
      assert(
          SmallHermesValue::canInlineDouble(+0.0) &&
          SmallHermesValue::canInlineDouble(-0.0) &&
          "+0.0/-0.0 must not be boxed");
      auto negativeZeroBits = static_cast<SearchType>(
          SmallHermesValue::encodeNumberValue(-0.0, runtime).getRaw());
      auto positiveZeroBits = static_cast<SearchType>(
          SmallHermesValue::encodeNumberValue(+0.0, runtime).getRaw());
      if (searchNum == 0) {
        // +0.0/-0.0 must match each other. Run two SIMD searches
        // and pick the earliest (forward) or latest (reverse) hit.
        if (reverse) {
          size_t s = 0, e = k + 1;
          int64_t r1 = fastSearch(rawData, s, e, negativeZeroBits, true);
          int64_t r2 = fastSearch(rawData, s, e, positiveZeroBits, true);
          int64_t result = std::max(r1, r2);
          if (result >= 0)
            return HermesValue::encodeTrustedNumberValue(result);
        } else {
          size_t s = k, e = len;
          int64_t r1 = fastSearch(rawData, s, e, negativeZeroBits, false);
          int64_t r2 = fastSearch(rawData, s, e, positiveZeroBits, false);
          int64_t result =
              (r1 < 0 || r2 < 0) ? std::max(r1, r2) : std::min(r1, r2);
          // If both r1 and r2 are -1, no zero is found.
          if (result >= 0)
            return HermesValue::encodeTrustedNumberValue(result);
        }
      } else {
        // Normal number (not 0, not NaN): raw-bit SIMD search.
        size_t s = reverse ? 0 : (size_t)k;
        size_t e = reverse ? (size_t)(k + 1) : len;
        int64_t result = fastSearch(rawData, s, e, rawTarget, reverse);
        if (result >= 0)
          return HermesValue::encodeTrustedNumberValue(result);
      }
    } else if (searchElementVal.isBoxedDouble()) {
      auto searchNum = searchElementVal.getBoxedDouble(runtime);
#define COMPARE_EXPR(element) \
  element.isBoxedDouble() && searchNum == element.getBoxedDouble(runtime)
      SEARCH_ARRAY
#undef COMPARE_EXPR
    } else if (searchElementVal.isString()) {
      auto searchStr = searchElementVal.getString(runtime);

#define COMPARE_EXPR(element) \
  element.isString() && searchStr->equals(element.getString(runtime))
      SEARCH_ARRAY
#undef COMPARE_EXPR
    } else if (searchElementVal.isBigInt()) {
      auto searchBigInt = searchElementVal.getBigInt(runtime);

#define COMPARE_EXPR(element) \
  element.isBigInt() && !searchBigInt->compare(element.getBigInt(runtime))
      SEARCH_ARRAY
#undef COMPARE_EXPR
    } else {
      // For all other types (e.g., Object), raw-bit SIMD search.
      size_t s = reverse ? 0 : (size_t)k;
      size_t e = reverse ? (size_t)(k + 1) : len;
      int64_t result = fastSearch(rawData, s, e, rawTarget, reverse);
      if (result >= 0)
        return HermesValue::encodeTrustedNumberValue(result);
    }

#undef SEARCH_ARRAY_DIRECTED
#undef SEARCH_ARRAY

    return HermesValue::encodeTrustedNumberValue(-1);
  }

  // Slow path for non-array objects or arrays with holes.
  lv.k = HermesValue::encodeTrustedNumberValue(k);
  auto marker = gcScope.createMarker();
  while (true) {
    gcScope.flushToMarker(marker);
    // Check that we're not done yet.
    if (!reverse) {
      if (lv.k->getDouble() >= len) {
        break;
      }
    } else {
      if (lv.k->getDouble() < 0) {
        break;
      }
    }
    ComputedPropertyDescWithSymStorage desc{lv.tmpPropNameStorage};
    JSObject::getComputedPrimitiveDescriptor(
        lv.obj, runtime, lv.k, lv.descObj, desc);
    CallResult<PseudoHandle<>> propRes = JSObject::getComputedPropertyValue_RJS(
        lv.obj, runtime, lv.descObj, desc.get(), lv.k);
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (!(*propRes)->isEmpty() &&
        strictEqualityTest(searchElement.get(), propRes->get())) {
      return lv.k.get();
    }
    // Update the index based on the direction of the search.
    lv.k = HermesValue::encodeTrustedNumberValue(
        lv.k->getDouble() + (reverse ? -1 : 1));
  }

  // Not found, return -1.
  return HermesValue::encodeTrustedNumberValue(-1);
}

CallResult<HermesValue> arrayPrototypeUnshift(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> lenProp;
    PinnedValue<> j;
    PinnedValue<> from;
    PinnedValue<> to;
    PinnedValue<> fromValue;
    PinnedValue<JSObject> fromDescObj;
    PinnedValue<SymbolID> fromNameTmpStorage;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScope gcScope(runtime);

  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*objRes);

  auto propRes = JSObject::getNamed_RJS(
      lv.O, runtime, Predefined::getSymbolID(Predefined::length));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.lenProp = std::move(*propRes);
  auto intRes = toLengthU64(runtime, lv.lenProp);
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t len = *intRes;
  size_t argCount = args.getArgCount();

  // 4. If argCount > 0, then
  if (argCount > 0) {
    // If len+ argCount > (2 ^ 53) -1, throw a TypeError exception.
    if (LLVM_UNLIKELY(len + argCount >= ((uint64_t)1 << 53) - 1)) {
      return runtime.raiseTypeError(
          "Array.prototype.unshift result out of space");
    }

    // Loop indices.
    uint64_t k = len;
    uint64_t j = 0;

    // Move elements to the right by argCount to account for the new elements.
    // TODO: Add a fast path for actual arrays.
    auto marker = gcScope.createMarker();
    while (k > 0) {
      gcScope.flushToMarker(marker);
      lv.from = HermesValue::encodeTrustedNumberValue(k - 1);
      lv.to = HermesValue::encodeTrustedNumberValue(k + argCount - 1);

      ComputedPropertyDescWithSymStorage fromDesc{lv.fromNameTmpStorage};
      JSObject::getComputedPrimitiveDescriptor(
          lv.O, runtime, lv.from, lv.fromDescObj, fromDesc);
      CallResult<PseudoHandle<>> propRes =
          JSObject::getComputedPropertyValue_RJS(
              lv.O, runtime, lv.fromDescObj, fromDesc.get(), lv.from);
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }

      if (LLVM_LIKELY(!(*propRes)->isEmpty())) {
        lv.fromValue = std::move(*propRes);
        if (LLVM_UNLIKELY(
                JSObject::putComputed_RJS(
                    lv.O,
                    runtime,
                    lv.to,
                    lv.fromValue,
                    PropOpFlags().plusThrowOnError()) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
      } else {
        // Shift the empty slot by deleting at the destination.
        if (LLVM_UNLIKELY(
                JSObject::deleteComputed(
                    lv.O, runtime, lv.to, PropOpFlags().plusThrowOnError()) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
      }
      k -= 1;
    }

    // Put the arguments into the beginning of the array.
    for (auto arg : args.handles()) {
      lv.j = HermesValue::encodeTrustedNumberValue(j);
      if (LLVM_UNLIKELY(
              JSObject::putComputed_RJS(
                  lv.O, runtime, lv.j, arg, PropOpFlags().plusThrowOnError()) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      gcScope.flushToMarker(marker);
      j += 1;
    }
  }

  // Increment length by argCount.
  auto newLen = HermesValue::encodeTrustedNumberValue(len + argCount);
  lv.lenProp = newLen;
  if (LLVM_UNLIKELY(
          JSObject::putNamed_RJS(
              lv.O,
              runtime,
              Predefined::getSymbolID(Predefined::length),
              lv.lenProp,
              PropOpFlags().plusThrowOnError()) == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return newLen;
}

CallResult<HermesValue> arrayPrototypeIndexOf(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return indexOfHelper(runtime, args, false);
}

CallResult<HermesValue> arrayPrototypeLastIndexOf(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return indexOfHelper(runtime, args, true);
}

/// Helper function for every/some.
/// \param every true if calling every(), false if calling some().
static inline CallResult<HermesValue>
everySomeHelper(Runtime &runtime, NativeArgs args, const bool every) {
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> lenProp;
    PinnedValue<> k;
    PinnedValue<> kValue;
    PinnedValue<JSObject> descObj;
    PinnedValue<SymbolID> tmpPropNameStorage;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScope gcScope(runtime);

  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*objRes);

  auto propRes = JSObject::getNamed_RJS(
      lv.O, runtime, Predefined::getSymbolID(Predefined::length));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.lenProp = std::move(*propRes);
  auto intRes = toLengthU64(runtime, lv.lenProp);
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t len = *intRes;

  auto callbackFn = args.dyncastArg<Callable>(0);
  if (!callbackFn) {
    return runtime.raiseTypeError(
        "Array.prototype.every() requires a callable argument");
  }

  // Loop through and run the callback.
  auto marker = gcScope.createMarker();
  // Index to check the callback on.
  uint64_t k = 0;
  while (k < len) {
    gcScope.flushToMarker(marker);

    ComputedPropertyDescWithSymStorage desc{lv.tmpPropNameStorage};
    lv.k = HermesValue::encodeTrustedNumberValue(k);
    JSObject::getComputedPrimitiveDescriptor(
        lv.O, runtime, lv.k, lv.descObj, desc);
    CallResult<PseudoHandle<>> propRes = JSObject::getComputedPropertyValue_RJS(
        lv.O, runtime, lv.descObj, desc, lv.k);
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (LLVM_LIKELY(!(*propRes)->isEmpty())) {
      // kPresent is true, call the callback on the kth element.
      lv.kValue = std::move(*propRes);
      auto callRes = Callable::executeCall3(
          callbackFn,
          runtime,
          args.getArgHandle(1),
          lv.kValue.get(),
          lv.k.get(),
          lv.O.getHermesValue());
      if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      auto testResult = std::move(*callRes);
      if (every) {
        // Done if one is false.
        if (!toBoolean(testResult.get())) {
          return HermesValue::encodeBoolValue(false);
        }
      } else {
        // Done if one is true.
        if (toBoolean(testResult.get())) {
          return HermesValue::encodeBoolValue(true);
        }
      }
    }

    k += 1;
  }

  // If we're looking for every, then we finished without returning true.
  // If we're looking for some, then we finished without returning false.
  return HermesValue::encodeBoolValue(every);
}

CallResult<HermesValue> arrayPrototypeEvery(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return everySomeHelper(runtime, args, true);
}

CallResult<HermesValue> arrayPrototypeSome(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return everySomeHelper(runtime, args, false);
}

CallResult<HermesValue> arrayPrototypeMap(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> lenProp;
    PinnedValue<JSArray> A;
    PinnedValue<> k;
    PinnedValue<JSObject> descObj;
    PinnedValue<SymbolID> tmpPropNameStorage;
    PinnedValue<> value;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  GCScope gcScope(runtime);
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(objRes.getValue());

  auto propRes = JSObject::getNamed_RJS(
      lv.O, runtime, Predefined::getSymbolID(Predefined::length));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.lenProp = std::move(*propRes);
  auto intRes = toLengthU64(runtime, lv.lenProp);
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t len = *intRes;

  auto callbackFn = args.dyncastArg<Callable>(0);
  if (!callbackFn) {
    return runtime.raiseTypeError(
        "Array.prototype.map() requires a callable argument");
  }

  // Resultant array.
  if (LLVM_UNLIKELY(len > std::numeric_limits<uint32_t>::max())) {
    return runtime.raiseRangeError("invalid array length");
  }
  auto arrRes = JSArray::create(runtime, len, len);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.A = std::move(*arrRes);

  // Current index to execute callback on.
  lv.k = HermesValue::encodeTrustedNumberValue(0);

  // Main loop to execute callback and store the results in A.
  // TODO: Implement a fast path for actual arrays.
  auto marker = gcScope.createMarker();
  while (lv.k->getDouble() < len) {
    gcScope.flushToMarker(marker);

    ComputedPropertyDescWithSymStorage desc{lv.tmpPropNameStorage};
    JSObject::getComputedPrimitiveDescriptor(
        lv.O, runtime, lv.k, lv.descObj, desc);
    CallResult<PseudoHandle<>> propRes = JSObject::getComputedPropertyValue_RJS(
        lv.O, runtime, lv.descObj, desc.get(), lv.k);
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (LLVM_LIKELY(!(*propRes)->isEmpty())) {
      // kPresent is true, execute callback and store result in A[k].
      auto kValue = std::move(*propRes);
      auto callRes = Callable::executeCall3(
          callbackFn,
          runtime,
          args.getArgHandle(1),
          kValue.get(),
          lv.k.get(),
          lv.O.getHermesValue());
      if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.value = std::move(*callRes);
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(
                  lv.A, runtime, lv.k->getDouble(), lv.value) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
    }

    lv.k = HermesValue::encodeTrustedNumberValue(lv.k->getDouble() + 1);
  }

  return lv.A.getHermesValue();
}

CallResult<HermesValue> arrayPrototypeFilter(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> lenProp;
    PinnedValue<JSArray> A;
    PinnedValue<> k;
    PinnedValue<> kValue;
    PinnedValue<JSObject> descObj;
    PinnedValue<SymbolID> tmpPropNameStorage;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  GCScope gcScope(runtime);
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*objRes);

  auto propRes = JSObject::getNamed_RJS(
      lv.O, runtime, Predefined::getSymbolID(Predefined::length));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.lenProp = std::move(*propRes);
  auto intRes = toLengthU64(runtime, lv.lenProp);
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t len = *intRes;

  auto callbackFn = args.dyncastArg<Callable>(0);
  if (!callbackFn) {
    return runtime.raiseTypeError(
        "Array.prototype.filter() requires a callable argument");
  }

  if (LLVM_UNLIKELY(len > std::numeric_limits<uint32_t>::max())) {
    return runtime.raiseRangeError("invalid array length");
  }
  auto arrRes = JSArray::create(runtime, len, 0);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.A = std::move(*arrRes);

  // Index in the original array.
  uint64_t k = 0;
  // Index to copy to in the new array.
  uint32_t to = 0;

  auto marker = gcScope.createMarker();
  while (k < len) {
    gcScope.flushToMarker(marker);

    ComputedPropertyDescWithSymStorage desc{lv.tmpPropNameStorage};
    lv.k = HermesValue::encodeTrustedNumberValue(k);
    JSObject::getComputedPrimitiveDescriptor(
        lv.O, runtime, lv.k, lv.descObj, desc);
    CallResult<PseudoHandle<>> propRes = JSObject::getComputedPropertyValue_RJS(
        lv.O, runtime, lv.descObj, desc.get(), lv.k);
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (LLVM_LIKELY(!(*propRes)->isEmpty())) {
      lv.kValue = std::move(*propRes);
      // Call the callback.
      auto callRes = Callable::executeCall3(
          callbackFn,
          runtime,
          args.getArgHandle(1),
          lv.kValue.get(),
          lv.k.get(),
          lv.O.getHermesValue());
      if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (toBoolean(callRes->get())) {
        // Add the element to the array if it passes the callback.
        if (LLVM_UNLIKELY(
                JSArray::setElementAt(lv.A, runtime, to, lv.kValue) ==
                ExecutionStatus::EXCEPTION))
          return ExecutionStatus::EXCEPTION;
        ++to;
      }
    }

    k += 1;
  }

  if (LLVM_UNLIKELY(
          JSArray::setLengthProperty(lv.A, runtime, to) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return lv.A.getHermesValue();
}

CallResult<HermesValue> arrayPrototypeFill(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> lenProp;
    PinnedValue<> value;
    PinnedValue<> k;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  GCScope gcScope(runtime);
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*objRes);
  // Get the length.
  auto propRes = JSObject::getNamed_RJS(
      lv.O, runtime, Predefined::getSymbolID(Predefined::length));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.lenProp = std::move(*propRes);
  auto lenRes = toLengthU64(runtime, lv.lenProp);
  if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double len = *lenRes;
  // Get the value to be filled.
  lv.value = args.getArg(0);
  // Get the relative start and end.
  auto intRes = toIntegerOrInfinity(runtime, args.getArgHandle(1));
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double relativeStart = intRes->getNumber();
  // Index to start the deletion/insertion at.
  double actualStart = relativeStart < 0 ? std::max(len + relativeStart, 0.0)
                                         : std::min(relativeStart, len);
  double relativeEnd;
  if (args.getArg(2).isUndefined()) {
    relativeEnd = len;
  } else {
    if (LLVM_UNLIKELY(
            (intRes = toIntegerOrInfinity(runtime, args.getArgHandle(2))) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    relativeEnd = intRes->getNumber();
  }
  // Actual end index.
  double actualEnd = relativeEnd < 0 ? std::max(len + relativeEnd, 0.0)
                                     : std::min(relativeEnd, len);
  lv.k = HermesValue::encodeTrustedNumberValue(actualStart);
  auto marker = gcScope.createMarker();
  while (lv.k->getDouble() < actualEnd) {
    if (LLVM_UNLIKELY(
            JSObject::putComputed_RJS(
                lv.O,
                runtime,
                lv.k,
                lv.value,
                PropOpFlags().plusThrowOnError()) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.k = HermesValue::encodeTrustedNumberValue(lv.k->getDouble() + 1);
    gcScope.flushToMarker(marker);
  }
  return lv.O.getHermesValue();
}

static CallResult<HermesValue>
findHelper(void *ctx, bool reverse, Runtime &runtime, NativeArgs args) {
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> lenProp;
    PinnedValue<> k;
    PinnedValue<> kValue;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  GCScope gcScope{runtime};
  bool findIndex = ctx != nullptr;
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*objRes);

  // Get the length.
  auto propRes = JSObject::getNamed_RJS(
      lv.O, runtime, Predefined::getSymbolID(Predefined::length));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.lenProp = std::move(*propRes);
  auto intRes = toLengthU64(runtime, lv.lenProp);
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t len = *intRes;

  auto predicate = args.dyncastArg<Callable>(0);
  if (!predicate) {
    return runtime.raiseTypeError("Find argument must be a function");
  }

  // "this" argument to the callback function.
  auto T = args.getArgHandle(1);
  auto marker = gcScope.createMarker();
  for (size_t i = 0; i < len; ++i) {
    uint64_t k = reverse ? len - i - 1 : i;
    lv.k = HermesValue::encodeTrustedNumberValue(k);
    gcScope.flushToMarker(marker);
    if (LLVM_UNLIKELY(
            (propRes = getIndexed_RJS(runtime, lv.O, k)) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.kValue = std::move(*propRes);
    auto callRes = Callable::executeCall3(
        predicate,
        runtime,
        T,
        lv.kValue.getHermesValue(),
        lv.k.getHermesValue(),
        lv.O.getHermesValue());
    if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    bool testResult = toBoolean(callRes->get());
    if (testResult) {
      // If this is index find variant, then return the index k.
      // Else, return the value at the index k.
      return findIndex ? lv.k.getHermesValue() : lv.kValue.getHermesValue();
    }
  }

  // Failure case for Array.prototype.findIndex is -1.
  // Failure case for Array.prototype.find is undefined.
  // The last variants share the same failure case values.
  return findIndex ? HermesValue::encodeTrustedNumberValue(-1)
                   : HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue> arrayPrototypeFind(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return findHelper(ctx, false, runtime, args);
}

CallResult<HermesValue> arrayPrototypeFindLast(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return findHelper(ctx, true, runtime, args);
}

/// Helper for reduce and reduceRight.
/// \param reverse set to true to reduceRight, false to reduce from the left.
static inline CallResult<HermesValue>
reduceHelper(Runtime &runtime, NativeArgs args, const bool reverse) {
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> lenProp;
    PinnedValue<JSObject> descObj;
    PinnedValue<SymbolID> nameTmpStorage;
    PinnedValue<> k;
    PinnedValue<> accumulator;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  GCScope gcScope(runtime);
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*objRes);

  auto propRes = JSObject::getNamed_RJS(
      lv.O, runtime, Predefined::getSymbolID(Predefined::length));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.lenProp = std::move(*propRes);
  auto intRes = toLengthU64(runtime, lv.lenProp);
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double len = *intRes;

  size_t argCount = args.getArgCount();

  auto callbackFn = args.dyncastArg<Callable>(0);
  if (!callbackFn) {
    return runtime.raiseTypeError(
        "Array.prototype.reduce() requires a callable argument");
  }

  // Can't reduce an empty array without an initial value.
  if (len == 0 && argCount < 2) {
    return runtime.raiseTypeError(
        "Array.prototype.reduce() requires an initial value with empty array");
  }

  // Current index in the reduction iteration.
  double k = reverse ? (len - 1) : 0;

  auto marker = gcScope.createMarker();

  // How much to increment k by each iteration of a loop.
  int increment = reverse ? -1 : 1;

  // Initialize the accumulator to either the intialValue arg or the first value
  // of the array.
  if (argCount >= 2) {
    lv.accumulator = args.getArg(1);
  } else {
    bool kPresent = false;
    while (!kPresent) {
      gcScope.flushToMarker(marker);
      if (!reverse) {
        if (k >= len) {
          break;
        }
      } else {
        if (k < 0) {
          break;
        }
      }
      ComputedPropertyDescWithSymStorage kDesc{lv.nameTmpStorage};
      lv.k = HermesValue::encodeTrustedNumberValue(k);
      JSObject::getComputedPrimitiveDescriptor(
          lv.O, runtime, lv.k, lv.descObj, kDesc);
      CallResult<PseudoHandle<>> propRes =
          JSObject::getComputedPropertyValue_RJS(
              lv.O, runtime, lv.descObj, kDesc.get(), lv.k);
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (LLVM_LIKELY(!(*propRes)->isEmpty())) {
        kPresent = true;
        lv.accumulator = std::move(*propRes);
      }
      k += increment;
    }
    if (!kPresent) {
      return runtime.raiseTypeError(
          "Array.prototype.reduce() requires an intial value with empty array");
    }
  }

  // Perform the reduce.
  while (true) {
    gcScope.flushToMarker(marker);
    if (!reverse) {
      if (k >= len) {
        break;
      }
    } else {
      if (k < 0) {
        break;
      }
    }

    ComputedPropertyDescWithSymStorage kDesc{lv.nameTmpStorage};
    lv.k = HermesValue::encodeTrustedNumberValue(k);
    JSObject::getComputedPrimitiveDescriptor(
        lv.O, runtime, lv.k, lv.descObj, kDesc);
    CallResult<PseudoHandle<>> propRes = JSObject::getComputedPropertyValue_RJS(
        lv.O, runtime, lv.descObj, kDesc.get(), lv.k);
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (LLVM_LIKELY(!(*propRes)->isEmpty())) {
      // kPresent is true, run the accumulation step.
      auto kValue = std::move(*propRes);
      auto callRes = Callable::executeCall4(
          callbackFn,
          runtime,
          Runtime::getUndefinedValue(),
          lv.accumulator.get(),
          kValue.get(),
          lv.k.get(),
          lv.O.getHermesValue());
      if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.accumulator = std::move(*callRes);
    }
    k += increment;
  }

  return lv.accumulator.get();
}

CallResult<HermesValue> arrayPrototypeReduce(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return reduceHelper(runtime, args, false);
}

CallResult<HermesValue> arrayPrototypeReduceRight(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return reduceHelper(runtime, args, true);
}

/// ES10.0 22.1.3.23.
CallResult<HermesValue> arrayPrototypeReverse(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // Fast path for a JSArray with a contiguous storage and no numeric props
  // in the prototype.
  if (vmisa<JSArray>(args.getThisArg())) {
    uint32_t len =
        JSArray::getLength(vmcast<JSArray>(args.getThisArg()), runtime);
    if (len <= 1)
      return args.getThisArg();

    if (arrayFastPathCheck(
            runtime,
            args.vmcastThis<JSArray>(),
            Runtime::makeNullHandle<HiddenClass>(),
            len)) {
      NoAllocScope noAllocScope{runtime};
      auto *storage = vmcast<JSArray>(args.getThisArg())
                          ->getIndexedStorageNullable(runtime);
      for (uint32_t l = 0, u = len - 1; l < u; ++l, --u) {
        assert(storage && "storage should not be null");
        auto lowerValue = storage->at(l);
        auto upperValue = storage->at(u);
        storage->set(l, upperValue, runtime.getHeap());
        storage->set(u, lowerValue, runtime.getHeap());
      }
      return args.getThisArg();
    }
  }

  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> lenProp;
    PinnedValue<> lower;
    PinnedValue<> upper;
    PinnedValue<> lowerValue;
    PinnedValue<> upperValue;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  GCScope gcScope(runtime);
  auto objRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*objRes);

  auto marker = gcScope.createMarker();

  auto propRes = JSObject::getNamed_RJS(
      lv.O, runtime, Predefined::getSymbolID(Predefined::length));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.lenProp = std::move(*propRes);
  auto lenRes = toLengthU64(runtime, lv.lenProp);
  if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t len = *lenRes;

  // Indices used in the reversal process.
  uint64_t middle = len / 2;
  uint64_t lower = 0;
  while (lower != middle) {
    gcScope.flushToMarker(marker);

    lv.upper = HermesValue::encodeTrustedNumberValue(len - lower - 1);
    lv.lower = HermesValue::encodeTrustedNumberValue(lower);
    CallResult<bool> lowerExistsRes =
        JSObject::hasComputed(lv.O, runtime, lv.lower);
    if (LLVM_UNLIKELY(lowerExistsRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (*lowerExistsRes) {
      CallResult<PseudoHandle<>> lowerValueRes =
          JSObject::getComputed_RJS(lv.O, runtime, lv.lower);
      if (LLVM_UNLIKELY(lowerValueRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.lowerValue = std::move(*lowerValueRes);
      gcScope.flushToMarker(marker);
    }

    CallResult<bool> upperExistsRes =
        JSObject::hasComputed(lv.O, runtime, lv.upper);
    if (LLVM_UNLIKELY(upperExistsRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (*upperExistsRes) {
      CallResult<PseudoHandle<>> upperValueRes =
          JSObject::getComputed_RJS(lv.O, runtime, lv.upper);
      if (LLVM_UNLIKELY(upperValueRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.upperValue = std::move(*upperValueRes);
      gcScope.flushToMarker(marker);
    }

    // Handle cases in which lower/upper do/don't exist.
    if (*lowerExistsRes && *upperExistsRes) {
      if (LLVM_UNLIKELY(
              JSObject::putComputed_RJS(
                  lv.O,
                  runtime,
                  lv.lower,
                  lv.upperValue,
                  PropOpFlags().plusThrowOnError()) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (LLVM_UNLIKELY(
              JSObject::putComputed_RJS(
                  lv.O,
                  runtime,
                  lv.upper,
                  lv.lowerValue,
                  PropOpFlags().plusThrowOnError()) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    } else if (*upperExistsRes) {
      if (LLVM_UNLIKELY(
              JSObject::putComputed_RJS(
                  lv.O,
                  runtime,
                  lv.lower,
                  lv.upperValue,
                  PropOpFlags().plusThrowOnError()) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (LLVM_UNLIKELY(
              JSObject::deleteComputed(
                  lv.O, runtime, lv.upper, PropOpFlags().plusThrowOnError()) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    } else if (*lowerExistsRes) {
      if (LLVM_UNLIKELY(
              JSObject::deleteComputed(
                  lv.O, runtime, lv.lower, PropOpFlags().plusThrowOnError()) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (LLVM_UNLIKELY(
              JSObject::putComputed_RJS(
                  lv.O,
                  runtime,
                  lv.upper,
                  lv.lowerValue,
                  PropOpFlags().plusThrowOnError()) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    }

    lower += 1;
  }

  return lv.O.getHermesValue();
}

CallResult<HermesValue> arrayPrototypeIncludes(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> lenProp;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScope gcScope{runtime};

  // 1. Let O be ? ToObject(this value).
  auto oRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(oRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*oRes);

  // Fast path: get array length directly for JSArray.
  uint64_t len;
  auto arrHandle = vmisa<JSArray>(*lv.O) ? Handle<JSArray>::vmcast(&lv.O)
                                         : Runtime::makeNullHandle<JSArray>();
  if (LLVM_LIKELY(arrHandle)) {
    len = JSArray::getLength(arrHandle.get(), runtime);
  } else {
    // 2. Let len be ? ToLength(? Get(O, "length")).
    auto lenPropRes = JSObject::getNamed_RJS(
        lv.O, runtime, Predefined::getSymbolID(Predefined::length));
    if (LLVM_UNLIKELY(lenPropRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.lenProp = std::move(*lenPropRes);
    auto lenRes = toLengthU64(runtime, lv.lenProp);
    if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    len = *lenRes;
  }

  // 3. If len is 0, return false.
  if (len == 0) {
    return HermesValue::encodeBoolValue(false);
  }

  // 4. Let n be ? ToIntegerOrInfinity(fromIndex).
  // (If fromIndex is undefined, this step produces the value 0.)
  auto nRes = toIntegerOrInfinity(runtime, args.getArgHandle(1));
  if (LLVM_UNLIKELY(nRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // Use double here, because ToInteger may return Infinity.
  double n = nRes->getNumber();

  uint64_t k;
  if (n >= 0) {
    // 5. If n ≥ 0, then
    // 5a. Let k be n.
    if (n > (double)len)
      k = len;
    else
      k = (uint64_t)n;
  } else {
    // 6. Else n < 0,
    // 6a. Let k be len + n.
    double fk = len + n;
    // 6b. If k < 0, let k be 0.
    if (fk < 0)
      fk = 0;
    k = (uint64_t)fk;
  }

  // Fast path.
  if (LLVM_LIKELY(
          arrHandle &&
          arrayFastPathCheck(
              runtime,
              arrHandle,
              Runtime::makeNullHandle<HiddenClass>(),
              (uint32_t)len))) {
    using SearchType = std::conditional_t<
        sizeof(SmallHermesValue::RawType) == 4,
        uint32_t,
        uint64_t>;
    auto searchElementVal =
        SmallHermesValue::encodeHermesValue(args.getArg(0), runtime);
    NoAllocScope noAlloc{runtime};
    assert(len != 0 && "we already checked len != 0");
    auto *arrStorage = arrHandle->getIndexedStorageUnsafe(runtime);

    llvh::ArrayRef<SearchType> rawData(
        reinterpret_cast<const SearchType *>(arrStorage->data()), len);
    auto rawTarget = static_cast<SearchType>(searchElementVal.getRaw());

    if (searchElementVal.isInlinedDouble()) {
      auto searchNum = searchElementVal.getNumber(runtime);
      if (LLVM_UNLIKELY(std::isnan(searchNum))) {
        // SameValueZero: NaN includes NaN. Scan for any NaN element.
        for (uint64_t i = k; i < len; ++i) {
          auto element = arrStorage->at(i);
          if (element.isInlinedDouble() &&
              std::isnan(element.getNumber(runtime)))
            return HermesValue::encodeBoolValue(true);
        }
      } else if (searchNum == 0) {
        // +0/-0: SameValueZero treats them as equal. Search for
        // +0.0 first, fall back to -0.0 if not found.
        assert(
            SmallHermesValue::canInlineDouble(+0.0) &&
            SmallHermesValue::canInlineDouble(-0.0) &&
            "+0.0/-0.0 must not be boxed");
        auto posZeroBits = static_cast<SearchType>(
            SmallHermesValue::encodeNumberValue(+0.0, runtime).getRaw());
        auto negZeroBits = static_cast<SearchType>(
            SmallHermesValue::encodeNumberValue(-0.0, runtime).getRaw());

        int64_t result = fastSearch(rawData, k, len, posZeroBits, false);
        if (result >= 0)
          return HermesValue::encodeBoolValue(true);
        result = fastSearch(rawData, k, len, negZeroBits, false);
        if (result >= 0)
          return HermesValue::encodeBoolValue(true);
      } else {
        // Normal number or NaN: all inlined NaNs share the same
        // raw encoding (quiet NaN), so raw-bit SIMD search works.
        int64_t result = fastSearch(rawData, k, len, rawTarget, false);
        if (result >= 0)
          return HermesValue::encodeBoolValue(true);
      }
    } else if (searchElementVal.isBoxedDouble()) {
      auto searchNum = searchElementVal.getBoxedDouble(runtime);
      if (LLVM_UNLIKELY(std::isnan(searchNum))) {
        // SameValueZero: NaN includes NaN. Scan for any NaN element.
        for (uint64_t i = k; i < len; ++i) {
          auto element = arrStorage->at(i);
          if (element.isBoxedDouble() &&
              std::isnan(element.getBoxedDouble(runtime)))
            return HermesValue::encodeBoolValue(true);
        }
      } else {
        for (uint64_t i = k; i < len; ++i) {
          auto element = arrStorage->at(i);
          if (element.isBoxedDouble() &&
              searchNum == element.getBoxedDouble(runtime))
            return HermesValue::encodeBoolValue(true);
        }
      }
    } else if (searchElementVal.isString()) {
      auto searchStr = searchElementVal.getString(runtime);
      for (uint64_t i = k; i < len; ++i) {
        auto element = arrStorage->at(i);
        if (element.isString() && searchStr->equals(element.getString(runtime)))
          return HermesValue::encodeBoolValue(true);
      }
    } else if (searchElementVal.isBigInt()) {
      auto searchBigInt = searchElementVal.getBigInt(runtime);
      for (uint64_t i = k; i < len; ++i) {
        auto element = arrStorage->at(i);
        if (element.isBigInt() &&
            !searchBigInt->compare(element.getBigInt(runtime)))
          return HermesValue::encodeBoolValue(true);
      }
    } else if (searchElementVal.isUndefined()) {
      // includes treats empty slots as undefined per spec.
      // Search for both undefined and empty raw encodings.
      int64_t result = fastSearch(rawData, k, len, rawTarget, false);
      if (result >= 0)
        return HermesValue::encodeBoolValue(true);
      auto emptyRaw = static_cast<SearchType>(
          SmallHermesValue::encodeEmptyValue().getRaw());
      result = fastSearch(rawData, k, len, emptyRaw, false);
      if (result >= 0)
        return HermesValue::encodeBoolValue(true);
    } else {
      // Objects, symbols, etc., SIMD raw-bit search.
      int64_t result = fastSearch(rawData, k, len, rawTarget, false);
      if (result >= 0)
        return HermesValue::encodeBoolValue(true);
    }

    return HermesValue::encodeBoolValue(false);
  }

  // 7. Repeat, while k < len
  auto marker = gcScope.createMarker();
  while (k < len) {
    gcScope.flushToMarker(marker);

    // 7a. Let elementK be the result of ? Get(O, ! ToString(k)).
    auto elementKRes = getIndexed_RJS(runtime, lv.O, k);
    if (LLVM_UNLIKELY(elementKRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // 7b. If SameValueZero(searchElement, elementK) is true, return true.
    if (isSameValueZero(args.getArg(0), elementKRes->get())) {
      return HermesValue::encodeBoolValue(true);
    }

    // 7c. Increase k by 1.
    ++k;
  }

  // 8. Return false.
  return HermesValue::encodeBoolValue(false);
}

/// ES14.0 23.1.3.33
CallResult<HermesValue> arrayPrototypeToReversed(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<JSArray> A;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // 1. Let O be ? ToObject(this value).
  auto oRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(oRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*oRes);

  // 2. Let len be ? LengthOfArrayLike(O).
  auto jsArr = vmisa<JSArray>(*lv.O) ? Handle<JSArray>::vmcast(&lv.O)
                                     : Runtime::makeNullHandle<JSArray>();
  auto lenRes = lengthOfArrayLike(runtime, lv.O, jsArr);
  if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto len = lenRes.getValue();

  // 3. Let A be ArrayCreate(len).
  uint32_t len32 = truncateToUInt32(len);
  if (LLVM_UNLIKELY(len32 != len)) {
    return runtime.raiseRangeError("invalid array length");
  }
  auto ARes = JSArray::create(runtime, 0, 0);
  if (LLVM_UNLIKELY(ARes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.A = std::move(*ARes);
  if (LLVM_UNLIKELY(
          JSArray::setStorageEndIndex(lv.A, runtime, len32) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_UNLIKELY(
          JSArray::setLengthProperty(lv.A, runtime, len32) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // Fast Path: Input is a JSArray and arrayFastPathCheck passes.
  if (jsArr &&
      arrayFastPathCheck(
          runtime, jsArr, Runtime::makeNullHandle<HiddenClass>(), len32)) {
    NoAllocScope noAllocScope{runtime};
    auto *srcStorage = jsArr->getIndexedStorageNullable(runtime);
    auto *destStorage = lv.A->getIndexedStorageNullable(runtime);
    for (uint32_t to = 0, from = len32 - 1; to < len32; ++to, --from) {
      assert(srcStorage && destStorage && "storage should not be null");
      SmallHermesValue fromValue = srcStorage->at(from);
      destStorage->set(
          to,
          LLVM_LIKELY(!fromValue.isEmpty())
              ? fromValue
              : SmallHermesValue::encodeUndefinedValue(),
          runtime.getHeap());
    }
    return lv.A.getHermesValue();
  }

  // Read a single element. This is a lambda to get around SmallHermesValue not
  // having an assignment operator.
  auto readElem =
      [&runtime, &jsArr, &lv](uint32_t from) -> CallResult<SmallHermesValue> {
    // 5c. Let fromValue be ? Get(O, from).
    if (LLVM_LIKELY(jsArr)) {
      auto elm = jsArr->at(runtime, from);
      // If the element is not empty, we can return it directly here.
      // Otherwise, we must proceed to the slow path.
      if (!elm.isEmpty())
        return elm;
    }

    // Slow path
    GCScopeMarkerRAII marker{runtime};
    // It is OK to have a PinnedValue here, because we are only storing a number
    // in it.
    PinnedValue<> fromV = HermesValue::encodeTrustedNumberValue(from);
    CallResult<PseudoHandle<>> propRes =
        JSObject::getComputed_RJS(lv.O, runtime, fromV);
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return SmallHermesValue::encodeHermesValue(
        propRes->getHermesValue(), runtime);
  };

  // 4. Let k be 0.
  uint32_t k = 0;
  // 5. Repeat, while k < len,
  while (k < len) {
    uint32_t from = len - k - 1;
    auto elem = readElem(from);
    if (LLVM_UNLIKELY(elem == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;

    JSArray::unsafeSetExistingElementAt(lv.A.get(), runtime, k, *elem);

    // 5e. Set k to k + 1.
    ++k;
  }

  return lv.A.getHermesValue();
}

/// ES2025 23.1.3.34
CallResult<HermesValue> arrayPrototypeToSorted(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  // 1. If comparator is not undefined and IsCallable(comparator) is false,
  // throw a TypeError exception.
  auto compareFn = Handle<Callable>::dyn_vmcast(args.getArgHandle(0));
  if (!args.getArg(0).isUndefined() && !compareFn) {
    return runtime.raiseTypeError("Array sort argument must be callable");
  }

  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<JSArray> A;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // 2. Let O be ? ToObject(this value).
  auto oRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(oRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*oRes);

  // 3. Let len be ? LengthOfArrayLike(O).
  auto jsArr = vmisa<JSArray>(*lv.O) ? Handle<JSArray>::vmcast(&lv.O)
                                     : Runtime::makeNullHandle<JSArray>();
  auto lenRes = lengthOfArrayLike(runtime, lv.O, jsArr);
  if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto len = lenRes.getValue();
  // We use a JSArray to store the sorted results, which is limited to
  // uint32_t elements.
  if (LLVM_UNLIKELY(len > UINT32_MAX)) {
    return runtime.raiseRangeError("invalid array length");
  }
  uint32_t len32 = (uint32_t)len;

  // 4. Let A be ? ArrayCreate(len).
  // Allocate with known capacity to avoid reallocation.
  auto ARes = JSArray::create(runtime, len32, 0);
  if (LLVM_UNLIKELY(ARes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.A = std::move(*ARes);
  if (len == 0) {
    return lv.A.getHermesValue();
  }

  // 6. Let sortedList be ? SortIndexedProperties(O, len, SortCompare,
  // READ-THROUGH-HOLES).
  // Copy all elements from O into A, treating holes as undefined.
  if (jsArr &&
      arrayFastPathCheck(
          runtime, jsArr, Runtime::makeNullHandle<HiddenClass>(), len32)) {
    arrayFastPathCopy(runtime, lv.A, jsArr, len32);
  } else {
    if (LLVM_UNLIKELY(
            JSArray::setStorageEndIndex(lv.A, runtime, len32) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // A is freshly created with the default class, so length is writable.
    auto newLen = SmallHermesValue::encodeNumberValue(len32, runtime);
    JSArray::putLengthUnsafe(lv.A.get(), runtime, newLen);
    GCScope gcScope{runtime};
    auto marker = gcScope.createMarker();
    for (uint32_t k = 0; k < len32; ++k) {
      gcScope.flushToMarker(marker);
      CallResult<PseudoHandle<>> propRes = getIndexed_RJS(runtime, lv.O, k);
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      auto shv = SmallHermesValue::encodeHermesValue(
          propRes->getHermesValue(), runtime);
      JSArray::unsafeSetExistingElementAt(lv.A.get(), runtime, k, shv);
    }
  }

  // Sort A in place. Use DirectSortModel since A is a fresh array that
  // user code has no reference to — direct storage access is safe.
  // 5. Let SortCompare be an Abstract Closure calling
  // CompareArrayElements(x, y, comparator).
  // 7-8. Copy sortedList into A (already done above, sort in place).
  DirectSortModel sm(runtime, lv.A, compareFn);
  if (LLVM_UNLIKELY(quickSort(&sm, 0u, len32) == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;

  // 9. Return A.
  return lv.A.getHermesValue();
}

/// Copies \p count elements from \p from (or \p fromArr if is simple array)
static inline CallResult<uint32_t> arrayCopyHelper(
    Runtime &runtime,
    GCScope &gcScope,
    Handle<JSObject> from,
    uint64_t fromStartIndex,
    Handle<JSArray> to,
    uint64_t toStartIndex,
    uint64_t count) {
  struct : Locals {
    PinnedValue<> fromValue;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  auto marker = gcScope.createMarker();
  uint64_t i = 0;

  while (i < count) {
    gcScope.flushToMarker(marker);

    auto propRes = getIndexed_RJS(runtime, from, fromStartIndex + i);
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    lv.fromValue = std::move(*propRes);

    PinnedValue<> toIndex =
        HermesValue::encodeTrustedNumberValue(toStartIndex + i);
    if (LLVM_UNLIKELY(
            JSObject::defineOwnComputedPrimitive(
                to,
                runtime,
                toIndex,
                DefinePropertyFlags::getDefaultNewPropertyFlags(),
                lv.fromValue,
                PropOpFlags().plusThrowOnError()) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    ++i;
  }

  return i;
}

/// ES14.0 23.1.3.35
CallResult<HermesValue> arrayPrototypeToSpliced(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<> insertIndex;
    PinnedValue<JSArray> A;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // 1. Let O be ? ToObject(this value).
  auto oRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(oRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*oRes);

  // 2. Let len be ? LengthOfArrayLike(O).
  auto lenRes = lengthOfArrayLike(
      runtime,
      lv.O,
      vmisa<JSArray>(*lv.O) ? Handle<JSArray>::vmcast(&lv.O)
                            : Runtime::makeNullHandle<JSArray>());
  if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t len = lenRes.getValue();

  // 3. Let relativeIndex be ? ToIntegerOrInfinity(index).
  auto relativeStartRes = toIntegerOrInfinity(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(relativeStartRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // Use double here, because ToInteger may return Infinity.
  double relativeStart = relativeStartRes->getNumber();

  // 4. If relativeStart is -∞, let actualStart be 0.
  // 5. Else if relativeStart < 0, let actualStart be max(len + relativeStart,
  // 0).
  // 6. Else, let actualStart be min(relativeStart, len).
  double actualStart = relativeStart < 0 ? std::max(len + relativeStart, 0.0)
                                         : std::min(relativeStart, (double)len);

  uint32_t argCount = args.getArgCount();
  uint64_t actualSkipCount;
  uint64_t insertCount;

  switch (argCount) {
    // 8. If start is not present, then
    case 0:
      insertCount = 0;
      // 8a. Let actualSkipCount be 0.
      actualSkipCount = 0;
      break;
    // 9. Else if skipCount is not present, then
    case 1:
      insertCount = 0;
      // 9a. Let actualSkipCount be len - actualStart.
      actualSkipCount = len - actualStart;
      break;
    // 10. Else
    default: {
      // 10a. Let sc be ? ToIntegerOrInfinity(skipCount).
      auto skipCountRes = toIntegerOrInfinity(runtime, args.getArgHandle(1));
      if (LLVM_UNLIKELY(skipCountRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }

      insertCount = argCount - 2;

      // 10b. Let actualSkipCount be the result of clamping dc between 0 and len
      // - actualStart.
      actualSkipCount =
          std::min(std::max(skipCountRes->getNumber(), 0.0), len - actualStart);
    }
  }

  // 11. Let newLen be len + insertCount - actualSkipCount.
  uint64_t lenAfterInsert = len + insertCount - actualSkipCount;

  // 12. If newLen > 2^53 - 1, throw a TypeError exception.
  if (LLVM_UNLIKELY(lenAfterInsert > (1LLU << 53) - 1)) {
    return runtime.raiseTypeError(
        "Array.prototype.toSpliced result out of space");
  }

  // 13. Let A be ArrayCreate(len).
  uint32_t lenAfterInsert32 = truncateToUInt32(lenAfterInsert);
  if (LLVM_UNLIKELY(lenAfterInsert32 != lenAfterInsert)) {
    return runtime.raiseRangeError("invalid array length");
  }
  auto ARes = JSArray::create(runtime, lenAfterInsert32, lenAfterInsert32);
  if (LLVM_UNLIKELY(ARes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.A = std::move(*ARes);

  // 14. Let i be 0
  double i = 0;

  // 15. Let r be actualStart + actualSkipCount.
  uint64_t r = actualStart + actualSkipCount;

  uint64_t paramIndex = 2;

  // 16a - 16d
  // Copy elements from original array O from beginning until actualStart into
  // new array A
  auto copyRes =
      arrayCopyHelper(runtime, gcScope, lv.O, 0, lv.A, 0, actualStart);
  if (LLVM_UNLIKELY(copyRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  i += copyRes.getValue();

  // 17. For each element E of items, do
  while (paramIndex < argCount) {
    // 17a. Let Pi be ! ToString(𝔽(i)).
    lv.insertIndex = HermesValue::encodeTrustedNumberValue(i);

    // 17b. Perform ! CreateDataPropertyOrThrow(A, Pi, E).
    if (LLVM_UNLIKELY(
            JSObject::defineOwnComputedPrimitive(
                lv.A,
                runtime,
                lv.insertIndex,
                DefinePropertyFlags::getDefaultNewPropertyFlags(),
                args.getArgHandle(paramIndex),
                PropOpFlags().plusThrowOnError()) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // 17c. Set i to i + 1.
    ++i;
    ++paramIndex;
  }

  // 18a - 18f
  // Copy remaining elements from original array O including skipCount into new
  // array A
  copyRes =
      arrayCopyHelper(runtime, gcScope, lv.O, r, lv.A, i, lenAfterInsert - i);
  if (LLVM_UNLIKELY(copyRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  i += copyRes.getValue();

  return lv.A.getHermesValue();
}

/// ES14.0 23.1.3.39
CallResult<HermesValue> arrayPrototypeWith(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> O;
    PinnedValue<JSArray> A;
    PinnedValue<> fromValue;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScope gcScope{runtime};

  // 1. Let O be ? ToObject(this value).
  auto oRes = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(oRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.O.castAndSetHermesValue<JSObject>(*oRes);

  // 2. Let len be ? LengthOfArrayLike(O).
  auto lenRes = lengthOfArrayLike(
      runtime,
      lv.O,
      vmisa<JSArray>(*lv.O) ? Handle<JSArray>::vmcast(&lv.O)
                            : Runtime::makeNullHandle<JSArray>());
  if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t len = lenRes.getValue();

  // 3. Let relativeIndex be ? ToIntegerOrInfinity(index).
  auto relativeIndexRes = toIntegerOrInfinity(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(relativeIndexRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // Use double here, because ToInteger may return Infinity.
  double relativeIndex = relativeIndexRes->getNumber();

  double actualIndex = 0;
  // 4. If relativeIndex ≥ 0, let actualIndex be relativeIndex.
  if (relativeIndex >= 0) {
    actualIndex = relativeIndex;
  }
  // 5. Else, let actualIndex be len + relativeIndex.
  else {
    actualIndex = len + relativeIndex;
  }

  // 6. If actualIndex ≥ len or actualIndex < 0, throw a RangeError exception.
  if (actualIndex < 0 || actualIndex >= len) {
    return runtime.raiseRangeError("invalid or out-of-range index");
  }

  // 7. Let A be ArrayCreate(len).
  uint32_t len32 = truncateToUInt32(len);
  if (LLVM_UNLIKELY(len32 != len)) {
    return runtime.raiseRangeError("invalid array length");
  }
  auto ARes = JSArray::create(runtime, len32, len32);
  if (LLVM_UNLIKELY(ARes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.A = std::move(*ARes);

  // 8. Let k be 0.
  uint64_t k = 0;

  auto marker = gcScope.createMarker();
  // 9. Repeat, while k < len,
  while (k < len) {
    gcScope.flushToMarker(marker);

    // 9a. Let Pk be ! ToString(𝔽(k)).
    // 9b. If k is actualIndex, let fromValue be value.
    if (k == actualIndex) {
      lv.fromValue = args.getArgHandle(1);
    }
    // 9c. Else, let fromValue be ? Get(O, Pk).
    else {
      auto propRes = getIndexed_RJS(runtime, lv.O, k);
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      lv.fromValue = std::move(*propRes);
    }

    // 9d. Perform ! CreateDataPropertyOrThrow(A, Pk, fromValue).
    PinnedValue<> kVal = HermesValue::encodeTrustedNumberValue(k);
    if (LLVM_UNLIKELY(
            JSObject::defineOwnComputedPrimitive(
                lv.A,
                runtime,
                kVal,
                DefinePropertyFlags::getDefaultNewPropertyFlags(),
                lv.fromValue,
                PropOpFlags().plusThrowOnError()) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // 9e. Set k to k + 1.
    ++k;
  }

  return lv.A.getHermesValue();
}

CallResult<HermesValue> arrayOf(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<JSObject> A;
    PinnedValue<> k;
    PinnedValue<> kValue;
    PinnedValue<> lenProp;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScope gcScope{runtime};

  // 1. Let len be the actual number of arguments passed to this function.
  uint32_t len = args.getArgCount();
  // 2. Let items be the List of arguments passed to this function.
  // 3. Let C be the this value.
  auto C = args.getThisHandle();

  // 4. If IsConstructor(C) is true, then
  if (isConstructor(runtime, *C)) {
    // a. Let A be Construct(C, «len»).
    lv.lenProp = HermesValue::encodeTrustedNumberValue(len);
    auto aRes = Callable::executeConstruct1(
        Handle<Callable>::vmcast(C), runtime, lv.lenProp);
    if (LLVM_UNLIKELY(aRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.A = PseudoHandle<JSObject>::vmcast(std::move(*aRes));
  } else {
    // 5. Else,
    // a. Let A be ArrayCreate(len).
    auto aRes = JSArray::create(runtime, len, len);
    if (LLVM_UNLIKELY(aRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.A = std::move(*aRes);
  }
  // 7. Let k be 0.
  lv.k = HermesValue::encodeTrustedNumberValue(0);

  GCScopeMarkerRAII marker{gcScope};
  // 8. Repeat, while k < len
  for (; lv.k->getNumberAs<uint32_t>() < len; marker.flush()) {
    // a. Let kValue be items[k].
    lv.kValue = args.getArg(lv.k->getNumber());

    // c. Let defineStatus be CreateDataPropertyOrThrow(A,Pk, kValue).
    if (LLVM_UNLIKELY(
            JSObject::defineOwnComputedPrimitive(
                lv.A,
                runtime,
                lv.k,
                DefinePropertyFlags::getDefaultNewPropertyFlags(),
                lv.kValue,
                PropOpFlags().plusThrowOnError()) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // e. Increase k by 1.
    lv.k = HermesValue::encodeTrustedNumberValue(lv.k->getNumber() + 1);
  }

  // 9. Let setStatus be Set(A, "length", len, true).
  // 10. ReturnIfAbrupt(setStatus).
  lv.lenProp = HermesValue::encodeTrustedNumberValue(len);
  auto setStatus = JSObject::putNamed_RJS(
      lv.A,
      runtime,
      Predefined::getSymbolID(Predefined::length),
      lv.lenProp,
      PropOpFlags().plusThrowOnError());
  if (LLVM_UNLIKELY(setStatus == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 11. Return A.
  return lv.A.getHermesValue();
}

/// ES6.0 22.1.2.1 Array.from ( items [ , mapfn [ , thisArg ] ] )
CallResult<HermesValue> arrayFrom(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : Locals {
    PinnedValue<Callable> mapfn;
    PinnedValue<> T;
    PinnedValue<JSObject> next;
    PinnedValue<> iteratorSym;
    PinnedValue<JSObject> A;
    PinnedValue<> usingIterator;
    PinnedValue<> k;
    PinnedValue<> mappedValue;
    PinnedValue<> nextValue;
    PinnedValue<> lenProp;
    PinnedValue<JSObject> arrayLike;
    CheckedIteratorRecordStorage iterStorage;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScope gcScope{runtime};
  auto itemsHandle = args.getArgHandle(0);
  // 1. Let C be the this value.
  auto C = args.getThisHandle();
  // 2. If mapfn is undefined, let mapping be false.
  // 3. else
  if (!args.getArg(1).isUndefined()) {
    auto mapfnArg = args.getArg(1);
    // a. If IsCallable(mapfn) is false, throw a TypeError exception.
    if (LLVM_UNLIKELY(!vmisa<Callable>(mapfnArg))) {
      return runtime.raiseTypeError("Mapping function is not callable.");
    }
    lv.mapfn.castAndSetHermesValue<Callable>(mapfnArg);
    // b. If thisArg was supplied, let T be thisArg; else let T be undefined.
    if (args.getArgCount() >= 3) {
      lv.T = args.getArg(2);
    }
    // c. Let mapping be true
  }
  // 4. Let usingIterator be GetMethod(items, @@iterator).
  // 5. ReturnIfAbrupt(usingIterator).
  lv.iteratorSym = HermesValue::encodeSymbolValue(
      Predefined::getSymbolID(Predefined::SymbolIterator));
  auto methodRes = getMethod(runtime, itemsHandle, lv.iteratorSym);
  if (LLVM_UNLIKELY(methodRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.usingIterator = std::move(*methodRes);

  // 6. If usingIterator is not undefined, then
  if (!lv.usingIterator->isUndefined()) {
    // a. If IsConstructor(C) is true, then
    if (isConstructor(runtime, *C)) {
      GCScopeMarkerRAII markerConstruct{gcScope};
      // i. Let A be Construct(C).
      auto callRes =
          Callable::executeConstruct0(Handle<Callable>::vmcast(C), runtime);
      if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.A = PseudoHandle<JSObject>::vmcast(std::move(*callRes));
    } else {
      // b. Else,
      //  i. Let A be ArrayCreate(0).
      auto arrRes = JSArray::create(runtime, 0, 0);
      if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.A = std::move(*arrRes);
    }
    // c. ReturnIfAbrupt(A).
    // d. Let iterator be GetIterator(items, usingIterator).
    // Assert we can cast usingIterator to a Callable otherwise getMethod would
    // have thrown.
    // e. ReturnIfAbrupt(iterator).
    auto iterRes = getCheckedIterator(
        runtime,
        args.getArgHandle(0),
        lv.iterStorage,
        Handle<Callable>::vmcast(&lv.usingIterator));
    if (LLVM_UNLIKELY(iterRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto iteratorRecord = *iterRes;
    // f. Let k be 0.
    lv.k = HermesValue::encodeTrustedNumberValue(0);
    // g. Repeat
    while (true) {
      GCScopeMarkerRAII marker1{runtime};
      // ii. Let next be IteratorStep(iteratorRecord).
      // iii. ReturnIfAbrupt(next).
      auto next = iteratorStep(runtime, iteratorRecord);
      if (LLVM_UNLIKELY(next == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      // iv. If next is false, then
      if (!*next) {
        // 1. Let setStatus be Set(A, "length", k, true).
        // 2. ReturnIfAbrupt(setStatus).
        // 3. Return A.
        auto setStatus = JSObject::putNamed_RJS(
            lv.A,
            runtime,
            Predefined::getSymbolID(Predefined::length),
            lv.k,
            PropOpFlags().plusThrowOnError());
        if (LLVM_UNLIKELY(setStatus == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        return lv.A.getHermesValue();
      }
      // v. Let nextValue be IteratorValue(next).
      // vi. ReturnIfAbrupt(nextValue).
      lv.next = std::move(*next);
      auto propRes = JSObject::getNamed_RJS(
          lv.next, runtime, Predefined::getSymbolID(Predefined::value));
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.nextValue = std::move(*propRes);
      // vii. If mapping is true, then
      if (lv.mapfn.get()) {
        // 1. Let mappedValue be Call(mapfn, T, «nextValue, k»).
        auto callRes = Callable::executeCall2(
            lv.mapfn, runtime, lv.T, *lv.nextValue, *lv.k);
        // 2. If mappedValue is an abrupt completion, return
        // IteratorClose(iterator, mappedValue).
        if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
          return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
        }
        // 3. Let mappedValue be mappedValue.[[value]].
        lv.mappedValue = std::move(*callRes);
      } else {
        // viii. Else, let mappedValue be nextValue.
        lv.mappedValue = lv.nextValue;
      }
      // ix. Let defineStatus be CreateDataPropertyOrThrow(A, Pk, mappedValue).
      // x. If defineStatus is an abrupt completion, return
      // IteratorClose(iterator, defineStatus).
      if (LLVM_UNLIKELY(
              JSObject::defineOwnComputedPrimitive(
                  lv.A,
                  runtime,
                  lv.k,
                  DefinePropertyFlags::getDefaultNewPropertyFlags(),
                  lv.mappedValue,
                  PropOpFlags().plusThrowOnError()) ==
              ExecutionStatus::EXCEPTION)) {
        return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
      }
      // xi. Increase k by 1.
      lv.k = HermesValue::encodeTrustedNumberValue(lv.k->getNumber() + 1);
    }
  }
  // 7. Assert: items is not an Iterable so assume it is an array-like object.
  // 8. Let arrayLike be ToObject(items).
  auto objRes = toObject(runtime, itemsHandle);
  // 9. ReturnIfAbrupt(arrayLike).
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.arrayLike.castAndSetHermesValue<JSObject>(*objRes);
  // 10. Let len be ToLength(Get(arrayLike, "length")).
  // 11. ReturnIfAbrupt(len).
  auto propRes = JSObject::getNamed_RJS(
      lv.arrayLike, runtime, Predefined::getSymbolID(Predefined::length));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.lenProp = std::move(*propRes);
  auto lengthRes = toLength(runtime, lv.lenProp);
  if (LLVM_UNLIKELY(lengthRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t len = lengthRes->getNumberAs<uint64_t>();
  // 12. If IsConstructor(C) is true, then
  if (isConstructor(runtime, *C)) {
    // a. Let A be Construct(C, «len»).
    lv.lenProp = std::move(*lengthRes);
    auto callRes = Callable::executeConstruct1(
        Handle<Callable>::vmcast(C), runtime, lv.lenProp);
    if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.A = PseudoHandle<JSObject>::vmcast(std::move(*callRes));
  } else {
    // 13. Else,
    //  a. Let A be ArrayCreate(len).
    uint32_t len32 = truncateToUInt32(len);
    if (LLVM_UNLIKELY(len32 != len)) {
      return runtime.raiseRangeError("invalid array length");
    }
    auto arrRes = JSArray::create(runtime, len, len);
    if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.A = std::move(*arrRes);
  }
  // 14. ReturnIfAbrupt(A).
  // 15. Let k be 0.
  lv.k = HermesValue::encodeTrustedNumberValue(0);
  // 16. Repeat, while k < len
  while (lv.k->getNumber() < (double)len) {
    GCScopeMarkerRAII marker2{runtime};
    // b. Let kValue be Get(arrayLike, Pk).
    propRes =
        getIndexed_RJS(runtime, lv.arrayLike, (uint64_t)lv.k->getNumber());
    // c. ReturnIfAbrupt(kValue).
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // d. If mapping is true, then
    if (lv.mapfn.get()) {
      // i. Let mappedValue be Call(mapfn, T, «kValue, k»).
      // ii. ReturnIfAbrupt(mappedValue).
      auto callRes = Callable::executeCall2(
          lv.mapfn, runtime, lv.T, propRes->get(), lv.k.getHermesValue());
      if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.mappedValue = std::move(*callRes);
    } else {
      // e. Else, let mappedValue be kValue.
      lv.mappedValue = std::move(*propRes);
    }
    // f. Let defineStatus be CreateDataPropertyOrThrow(A, Pk, mappedValue).
    // g. ReturnIfAbrupt(defineStatus).
    if (LLVM_UNLIKELY(
            JSObject::defineOwnComputedPrimitive(
                lv.A,
                runtime,
                lv.k,
                DefinePropertyFlags::getDefaultNewPropertyFlags(),
                lv.mappedValue,
                PropOpFlags().plusThrowOnError()) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // h. Increase k by 1.
    lv.k = HermesValue::encodeTrustedNumberValue(lv.k->getNumber() + 1);
  }
  // 17. Let setStatus be Set(A, "length", len, true).
  auto setStatus = JSObject::putNamed_RJS(
      lv.A,
      runtime,
      Predefined::getSymbolID(Predefined::length),
      lv.k,
      PropOpFlags().plusThrowOnError());
  // 18. ReturnIfAbrupt(setStatus).
  if (LLVM_UNLIKELY(setStatus == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 19. Return A.
  return lv.A.getHermesValue();
}

} // namespace vm
} // namespace hermes

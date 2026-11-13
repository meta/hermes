/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES7 22.2 TypedArray
//===----------------------------------------------------------------------===//
#include "JSLibInternal.h"
#include "hermes/Support/FastArraySearch.h"
#include "hermes/VM/ArrayStorage.h"
#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/JSLib.h"
#include "hermes/VM/JSLib/Base64Util.h"
#include "hermes/VM/JSLib/Sorting.h"
#include "hermes/VM/JSTypedArray.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/StringBuilder.h"
#include "hermes/VM/StringView.h"

#include <type_traits>

namespace hermes {
namespace vm {

namespace {

/// @name Abstract Operations and Helper functions
/// @{

/// IterableToArrayLike for an array \p sourceArray whose @@iterator is the
/// builtin Array.prototype.values, when %ArrayIteratorPrototype%.next is the
/// builtin next too. Iterating such an array only reads its elements by index,
/// so read them directly, without creating the iterator and an iterator result
/// object for every element.
/// \return a new JSArray with the values that the iteration would produce.
CallResult<HermesValue> iterableToArrayLikeFastPath(
    Runtime &runtime,
    Handle<JSArray> sourceArray) {
  struct : public Locals {
    PinnedValue<> nextValue;
    PinnedValue<JSArray> A;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  uint32_t length = JSArray::getLength(*sourceArray, runtime);
  // JSArray fast path: copy the elements directly from indexed storage.
  if (arrayFastPathCheck(
          runtime,
          sourceArray,
          Runtime::makeNullHandle<HiddenClass>(),
          length)) {
    auto arrRes = JSArray::create(runtime, length, 0);
    if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.A = std::move(*arrRes);
    arrayFastPathCopy(runtime, lv.A, sourceArray, length);
    return lv.A.getHermesValue();
  }

  auto arrRes = JSArray::create(runtime, 0, 0);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.A = std::move(*arrRes);

  // Like the iterator, read the length again before every element, since a
  // getter may have changed it.
  uint32_t k = 0;
  while (k < JSArray::getLength(*sourceArray, runtime)) {
    GCScopeMarkerRAII marker{runtime};

    // Get(array, k). Use the value in indexed storage if there is one. Holes,
    // accessors and elements stored as named properties need the full Get,
    // which may run a getter.
    lv.nextValue = sourceArray->at(runtime, k).unboxToHV(runtime);
    if (LLVM_UNLIKELY(lv.nextValue->isEmpty())) {
      auto valueRes = getIndexed_RJS(runtime, sourceArray, k);
      if (LLVM_UNLIKELY(valueRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.nextValue = std::move(*valueRes);
    }

    if (LLVM_UNLIKELY(
            JSArray::setElementAt(lv.A, runtime, k, lv.nextValue) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    ++k;
  }

  // Update the dest array length.
  if (LLVM_UNLIKELY(
          JSArray::setLengthProperty(lv.A, runtime, k) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return lv.A.getHermesValue();
}

/// ES7 22.2.2.1.1 IterableToArrayLike
/// If items has an @@iterator method, iterate through it and collect
/// values into an array. Otherwise, return ToObject(items).
CallResult<HermesValue> iterableToArrayLike(Runtime &runtime, Handle<> items) {
  struct : public Locals {
    PinnedValue<> iteratorSymbol;
    PinnedValue<> usingIterator;
    PinnedValue<> nextValue;
    PinnedValue<JSArray> A;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  GCScope gcScope{runtime};

  // 1. Let usingIterator be ? GetMethod(items, @@iterator).
  lv.iteratorSymbol = HermesValue::encodeSymbolValue(
      Predefined::getSymbolID(Predefined::SymbolIterator));
  auto methodRes = getMethod(runtime, items, lv.iteratorSymbol);
  if (LLVM_UNLIKELY(methodRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.usingIterator = std::move(*methodRes);

  // 2. If usingIterator is undefined, items is not iterable, so assume it is
  // an array-like object and return ToObject(items).
  if (lv.usingIterator->isUndefined()) {
    return toObject(runtime, items);
  }

  // Avoid allocating iterator and iterator-result objects for ordinary arrays.
  // Both methods must be unmodified because the generic iterator protocol
  // observes each of them.
  auto sourceArray = Handle<JSArray>::dyn_vmcast(items);
  if (sourceArray && lv.usingIterator->isObject() &&
      lv.usingIterator->getObject() == *runtime.arrayPrototypeValues) {
    auto iteratorPrototype =
        Handle<JSObject>::vmcast(&runtime.arrayIteratorPrototype);
    NamedPropertyDescriptor desc;
    JSObject *propObj = JSObject::getNamedDescriptorPredefined(
        iteratorPrototype, runtime, Predefined::next, desc);
    if (LLVM_LIKELY(propObj) && LLVM_LIKELY(!desc.flags.accessor) &&
        LLVM_LIKELY(!desc.flags.proxyObject) &&
        LLVM_LIKELY(!desc.flags.hostObject)) {
      // `next` must be the builtin %ArrayIteratorPrototype%.next,
      // which is the native function arrayIteratorPrototypeNext with no
      // context.
      SmallHermesValue nextMethod =
          JSObject::getNamedSlotValueUnsafe(propObj, runtime, desc);
      auto *nativeNext = nextMethod.isObject()
          ? dyn_vmcast<NativeFunction>(nextMethod.getObject(runtime))
          : nullptr;
      if (LLVM_LIKELY(nativeNext) &&
          LLVM_LIKELY(
              nativeNext->getFunctionPtr() == arrayIteratorPrototypeNext) &&
          LLVM_LIKELY(nativeNext->getContext() == nullptr)) {
        return iterableToArrayLikeFastPath(runtime, sourceArray);
      }
    }
  }

  // 3. Let iteratorRecord be ? GetIterator(items, sync, usingIterator).
  auto iterRes = getCheckedIterator(
      runtime, items, Handle<Callable>::vmcast(&lv.usingIterator));
  if (LLVM_UNLIKELY(iterRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto iteratorRecord = *iterRes;

  // 4. Let values be a new empty List.
  auto arrRes = JSArray::create(runtime, 0, 0);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
  }
  lv.A = std::move(*arrRes);

  // 5. Repeat,
  uint32_t k = 0;
  while (true) {
    GCScopeMarkerRAII marker{gcScope};

    // Get the next value. As in IteratorToList, errors from next(), done or
    // value propagate without closing the iterator.
    auto stepRes = iteratorStepValue(runtime, iteratorRecord, &lv.nextValue);
    if (LLVM_UNLIKELY(stepRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // If done, return the array.
    if (!*stepRes) {
      if (LLVM_UNLIKELY(
              JSArray::setLengthProperty(lv.A, runtime, k) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      return lv.A.getHermesValue();
    }

    // Check for overflow before appending.
    if (LLVM_UNLIKELY(k == UINT32_MAX)) {
      (void)runtime.raiseRangeError("iterable too long for TypedArray");
      return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
    }

    // Append to array.
    if (LLVM_UNLIKELY(
            JSArray::setElementAt(lv.A, runtime, k, lv.nextValue) ==
            ExecutionStatus::EXCEPTION)) {
      return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
    }
    ++k;
  }
}

/// Given a numeric \p value and \p length, returns either length + value if
/// value is negative, or value if it is positive.
/// The returned value is always in the range [0, length].
template <typename T>
T convertNegativeBoundsRelativeToLength(T value, T length) {
  // To avoid casting.
  T zero = 0;
  return value < 0 ? std::max(length + value, zero) : std::min(value, length);
}

/// ES7 22.2.4.6
ExecutionStatus typedArrayCreate(
    Runtime &runtime,
    Handle<Callable> constructor,
    uint64_t length,
    MutableHandle<JSTypedArrayBase> result) {
  struct : public Locals {
    PinnedValue<> lengthValue;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.lengthValue = HermesValue::encodeTrustedNumberValue(length);
  auto callRes =
      Callable::executeConstruct1(constructor, runtime, lv.lengthValue);
  if (callRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  PseudoHandle<> retval = std::move(*callRes);
  if (!vmisa<JSTypedArrayBase>(retval.get())) {
    return runtime.raiseTypeError(
        "The constructor needs to construct a TypedArray");
  }
  result.castAndSetHermesValue<JSTypedArrayBase>(retval.get());
  // If `argumentList` is a single number, then
  // If the value of newTypedArray's [[ArrayLength]] internal slot <
  // argumentList[0], throw a TypeError exception.
  if (LLVM_UNLIKELY(result->getLength() < length)) {
    return runtime.raiseTypeError(
        "TypedArray constructor created an array that was too small");
  }
  return ExecutionStatus::RETURNED;
}

/// @name %JSTypedArray%
/// @{

// ES 2018 22.2.1.2
template <typename T, CellKind C>
CallResult<HermesValue> typedArrayConstructorFromLength(
    Runtime &runtime,
    Handle<JSTypedArray<T, C>> self,
    Handle<> length) {
  auto resIndex = toIndex(runtime, length);
  if (resIndex == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  if (JSTypedArray<T, C>::createBuffer(
          runtime, self, resIndex.getValue().getNumberAs<uint64_t>()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return self.getHermesValue();
}

/// \return true if \p kind is a BigInt typed array.
static bool isBigIntTypedArrayKind(CellKind kind) {
  return kind == CellKind::BigInt64ArrayKind ||
      kind == CellKind::BigUint64ArrayKind;
}

// ES6 22.2.1.3
template <typename T, CellKind C>
CallResult<HermesValue> typedArrayConstructorFromTypedArray(
    Runtime &runtime,
    Handle<JSTypedArray<T, C>> self,
    Handle<JSTypedArrayBase> other) {
  if (!other->attached(runtime)) {
    return runtime.raiseTypeError(
        "Cannot construct a TypedArray from a detached TypedArray");
  }
  // If srcArray.[[ContentType]] is not O.[[ContentType]], throw a TypeError
  // exception. The internal property [[ContentType]] is defined to be either
  // BigInt for BigInt64Array/BigUint64Array or Number for all other types.
  if (isBigIntTypedArrayKind(C) != isBigIntTypedArrayKind(other->getKind())) {
    return runtime.raiseTypeError("Cannot mix BigInt and other types");
  }
  if (JSTypedArray<T, C>::createBuffer(runtime, self, other->getLength()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  if (JSTypedArrayBase::setToCopyOfTypedArray(
          runtime, self, 0, other, 0, other->getLength()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return self.getHermesValue();
}

/// ES 2025 23.2.5.1.3 InitializeTypedArrayFromArrayBuffer ( O, buffer,
/// byteOffset, length )
template <typename T, CellKind C>
CallResult<HermesValue> typedArrayConstructorFromArrayBuffer(
    Runtime &runtime,
    Handle<JSTypedArray<T, C>> self,
    Handle<JSArrayBuffer> buffer,
    Handle<> byteOffset,
    Handle<> length) {
  // 1. Let elementSize be TypedArrayElementSize(O). This is sizeof(T).
  // 2. Let offset be ? ToIndex(byteOffset).
  auto res = toIndex(runtime, byteOffset);
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t offset = res->getNumberAs<uint64_t>();
  // 3. If offset modulo elementSize ≠ 0, throw a RangeError exception.
  if (offset % sizeof(T) != 0) {
    return runtime.raiseRangeError(
        "new TypedArray(buffer, [byteOffset], "
        "[length]): if byteOffset is specified, it "
        "must be evenly divisible by the element size");
  }
  // 4. Resizable ArrayBuffer case: unsupported.
  // 7. Let bufferByteLength be ArrayBufferByteLength(buffer, seq-cst).
  // 8. Resizable ArrayBuffer case: unsupported.
  auto bufferByteLength = buffer->size();
  uint64_t newByteLength = 0;
  if (length->isUndefined()) {
    // 6. If IsDetachedBuffer(buffer) is true, throw a TypeError exception.
    if (!buffer->attached()) {
      return runtime.raiseTypeError(
          "Cannot construct a TypedArray from a detached ArrayBuffer");
    }
    // 9a. If length is undefined, then
    //   i. If bufferByteLength modulo elementSize ≠ 0, throw a RangeError
    //   exception. ii. Let newByteLength be bufferByteLength - offset. iii. If
    //   newByteLength < 0, throw a RangeError exception.
    if (bufferByteLength % sizeof(T) != 0) {
      return runtime.raiseRangeError(
          "new TypedArray(buffer, [byteOffset], "
          "[length]): buffer's size must be evenly "
          "divisible by the element size");
    }
    if (bufferByteLength < offset) {
      return runtime.raiseRangeError(
          "new TypedArray(buffer, [byteOffset], "
          "[length]): byteOffset must be less than "
          "buffer.byteLength");
    }
    newByteLength = bufferByteLength - offset;
  } else {
    // 5. If length is not undefined, then
    //   a. Let newLength be ? ToIndex(length).
    auto res2 = toIndex(runtime, length);
    if (res2 == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    // 6. If IsDetachedBuffer(buffer) is true, throw a TypeError exception.
    if (!buffer->attached()) {
      return runtime.raiseTypeError(
          "Cannot construct a TypedArray from a detached ArrayBuffer");
    }
    // 9b. Else,
    //   i. Let newByteLength be newLength × elementSize.
    //   ii. If offset + newByteLength > bufferByteLength, throw a RangeError
    //   exception.
    uint64_t newLength = res2->getNumberAs<uint64_t>();
    newByteLength = newLength * sizeof(T);
    if (offset + newByteLength > bufferByteLength) {
      return runtime.raiseRangeError(
          "new TypedArray(buffer, [byteOffset], [length]): byteOffset + "
          "length * elementSize must be less than buffer.byteLength");
    }
  }
  JSTypedArrayBase::setBuffer(
      runtime, *self, *buffer, offset, newByteLength, sizeof(T));
  return self.getHermesValue();
}

// ES7 22.2.4.4
template <typename T, CellKind C>
CallResult<HermesValue> typedArrayConstructorFromObject(
    Runtime &runtime,
    Handle<JSTypedArray<T, C>> self,
    Handle<> obj) {
  struct : public Locals {
    PinnedValue<JSObject> arrayLike;
    PinnedValue<> propValue;
    PinnedValue<> kValue;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Steps 1 & 2 already covered by caller.
  // 5. Let arrayLike be ? IterableToArrayLike(object).
  auto objRes = iterableToArrayLike(runtime, obj);
  if (objRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.arrayLike.template castAndSetHermesValue<JSObject>(objRes.getValue());
  // 6. Let len be ? ToLength(? Get(arrayLike, "length")).
  auto propRes = JSObject::getNamed_RJS(
      lv.arrayLike, runtime, Predefined::getSymbolID(Predefined::length));
  if (propRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.propValue = std::move(*propRes);
  auto intRes = toLength(runtime, lv.propValue);
  if (intRes == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  uint64_t len = intRes->template getNumberAs<uint64_t>();
  // 4. Let O be ? AllocateTypedArray(constructorName, NewTarget,
  // "%TypedArrayPrototype%").
  // 7. Perform ? AllocateTypedArrayBuffer(O, len).
  if (JSTypedArray<T, C>::createBuffer(runtime, self, len) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  GCScope scope(runtime);
  // 8. Let k be 0.
  // Note: createBuffer validates that len fits in size_type.
  JSTypedArrayBase::size_type i = 0;
  auto marker = scope.createMarker();
  // 9. Repeat, while k < len.
  for (; i < len; ++i) {
    // a. Let Pk be ! ToString(k).
    // b. Let kValue be ? Get(arrayLike, Pk).
    // c. Perform ? Set(O, Pk, kValue, true).
    if ((propRes = getIndexed_RJS(runtime, lv.arrayLike, i)) ==
        ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    lv.kValue = std::move(*propRes);
    if (JSObject::setOwnIndexed(self, runtime, i, lv.kValue) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    scope.flushToMarker(marker);
    // d. Increase k by 1.
  }
  // 10. Return O.
  return self.getHermesValue();
}

template <typename T, CellKind C>
CallResult<HermesValue> typedArrayConstructor(
    void *,
    Runtime &runtime,
    NativeArgs args,
    const PinnedValue<NativeConstructor> *arrayConstructor,
    const PinnedValue<JSObject> *arrayPrototype,
    const PinnedValue<HiddenClass> *arrayClazz) {
  // 1. If NewTarget is undefined, throw a TypeError exception.
  if (!args.isConstructorCall()) {
    return runtime.raiseTypeError(
        "JSTypedArray() called in function context instead of constructor");
  }

  struct : public Locals {
    PinnedValue<JSObject> selfParent;
    PinnedValue<HiddenClass> clazz;
    PinnedValue<JSTypedArray<T, C>> self;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  if (LLVM_LIKELY(
          args.getNewTarget().getRaw() ==
          arrayConstructor->getHermesValue().getRaw())) {
    lv.selfParent = *arrayPrototype;
    lv.clazz = *arrayClazz;
  } else {
    CallResult<PseudoHandle<JSObject>> thisParentRes =
        NativeConstructor::parentForNewThis_RJS(
            runtime,
            Handle<Callable>::vmcast(&args.getNewTarget()),
            *arrayPrototype);
    if (LLVM_UNLIKELY(thisParentRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.selfParent = std::move(*thisParentRes);
    lv.clazz = runtime.getHiddenClassForPrototype(*lv.selfParent, *arrayClazz);
  }
  assert(*lv.clazz && "must have a hidden class");
  lv.self = JSTypedArray<T, C>::create(runtime, lv.selfParent, lv.clazz);
  if (args.getArgCount() == 0) {
    // ES6 22.2.1.1
    if (JSTypedArray<T, C>::createBuffer(runtime, lv.self, 0) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    return lv.self.getHermesValue();
  }
  auto firstArg = args.getArgHandle(0);
  if (!firstArg->isObject()) {
    return typedArrayConstructorFromLength<T, C>(runtime, lv.self, firstArg);
  }
  if (auto otherTA = Handle<JSTypedArrayBase>::dyn_vmcast(firstArg)) {
    return typedArrayConstructorFromTypedArray<T, C>(runtime, lv.self, otherTA);
  }
  if (auto buffer = Handle<JSArrayBuffer>::dyn_vmcast(firstArg)) {
    return typedArrayConstructorFromArrayBuffer<T, C>(
        runtime, lv.self, buffer, args.getArgHandle(1), args.getArgHandle(2));
  }
  return typedArrayConstructorFromObject<T, C>(runtime, lv.self, firstArg);
}

template <typename T, CellKind C, NativeFunctionPtr Ctor>
HermesValue createTypedArrayConstructor(Runtime &runtime) {
  struct : public Locals {
    PinnedValue<> bytesPerElement;
    PinnedValue<NativeConstructor> cons;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  using TA = JSTypedArray<T, C>;
  auto proto = TA::getPrototype(runtime);

  defineSystemConstructor(
      runtime,
      TA::getName(runtime),
      Ctor,
      proto,
      Handle<JSObject>::vmcast(&runtime.typedArrayBaseConstructor),
      3,
      lv.cons);

  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.enumerable = 0;
  dpf.configurable = 0;
  dpf.writable = 0;

  lv.bytesPerElement = HermesValue::encodeTrustedNumberValue(sizeof(T));
  // %TypedArray%.prototype.xxx.
  defineProperty(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::BYTES_PER_ELEMENT),
      lv.bytesPerElement,
      dpf);

  // %TypedArray%.xxx.
  defineProperty(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::BYTES_PER_ELEMENT),
      lv.bytesPerElement,
      dpf);
  return lv.cons.getHermesValue();
}

/// Implements the loop for map and filter. Template parameter \p MapOrFilter
/// should be true for map, and false for filter.
template <bool MapOrFilter>
CallResult<HermesValue> mapFilterLoop(
    Runtime &runtime,
    Handle<JSTypedArrayBase> self,
    Handle<Callable> callbackfn,
    Handle<> thisArg,
    Handle<JSArray> values,
    JSTypedArrayBase::size_type insert,
    JSTypedArrayBase::size_type len) {
  struct : public Locals {
    PinnedValue<> storage;
    PinnedValue<> val;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  GCScopeMarkerRAII marker{runtime};
  for (JSTypedArrayBase::size_type i = 0; i < len; ++i) {
    lv.val =
        JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, i);
    auto callRes = Callable::executeCall3(
        callbackfn,
        runtime,
        thisArg,
        lv.val.getHermesValue(),
        HermesValue::encodeTrustedNumberValue(i),
        self.getHermesValue());
    if (callRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    if (MapOrFilter) {
      // Map adds the result of the callback onto the array.
      lv.storage = std::move(*callRes);
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(values, runtime, insert++, lv.storage) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
    } else if (toBoolean(callRes->get())) {
      lv.storage = lv.val.getHermesValue();
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(values, runtime, insert++, lv.storage) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
    }
    marker.flush();
  }
  return HermesValue::encodeTrustedNumberValue(insert);
}

/// Sort model for TypedArray.prototype.sort with a user-provided compareFn.
/// For TypedArray of BigInt64/BigUint64, we should construct it with an
/// ArrayStorage; for TypedArray of other kinds, we should pass a
/// JSTypedArrayBase. Note that we could make it templated on the input type,
/// but it does not worth the extra binary size.
class TypedArraySortModel : public SortModel {
 protected:
  /// Runtime to sort in.
  Runtime &runtime_;

  /// Scope to allocate handles in, gets destroyed with this.
  GCScope gcScope_;

  /// JS comparison function, return -1 for less, 0 for equal, 1 for greater.
  Handle<Callable> compareFn_;

  /// Captured elements to sort. It should be either a JSTypedArrayBase or
  /// ArrayStorage.
  Handle<> items_;

  /// Whether \c items_ is a JSTypedArrayBase.
  bool inputIsTypedArray_;

  struct : Locals {
    PinnedValue<> a;
    PinnedValue<> b;
    PinnedValue<> callResult;
  } lv_;
  LocalsRAII lraii_;

  /// Marker created after initializing all fields so handles allocated later
  /// can be flushed.
  GCScope::Marker gcMarker_;

  /// Get the element at index from items_. This does not allocate.
  HermesValue getElement(uint32_t index) const {
    if (inputIsTypedArray_) {
      JSTypedArrayBase *typedArray = vmcast<JSTypedArrayBase>(items_.get());
      return JSTypedArrayBase::polyReadNoAlloc(typedArray, runtime_, index);
    }
    ArrayStorage *arrStorage = vmcast<ArrayStorage>(items_.get());
    return arrStorage->at(index);
  }

  /// Set the element at index of items_ to \p value. This does not allocate.
  void setElement(uint32_t index, HermesValue value) {
    if (inputIsTypedArray_) {
      JSTypedArrayBase *typedArray = vmcast<JSTypedArrayBase>(items_.get());
      return JSTypedArrayBase::polyWriteNoAlloc(
          typedArray, runtime_, index, value);
    }
    ArrayStorage *arrStorage = vmcast<ArrayStorage>(items_.get());
    return arrStorage->set(index, value, runtime_.getHeap());
  }

 public:
  TypedArraySortModel(
      Runtime &runtime,
      Handle<> items,
      Handle<Callable> compareFn)
      : runtime_(runtime),
        gcScope_(runtime),
        compareFn_(compareFn),
        items_(items),
        lraii_(runtime, &lv_),
        gcMarker_(gcScope_.createMarker()) {
    inputIsTypedArray_ = vmisa<JSTypedArrayBase>(items_.get());
    assert(inputIsTypedArray_ || vmisa<ArrayStorage>(items_.get()));
#ifndef NDEBUG
    if (inputIsTypedArray_) {
      auto *typedArray = vmcast<JSTypedArrayBase>(*items_);
      assert(
          (typedArray->getKind() != CellKind::BigInt64ArrayKind &&
           typedArray->getKind() != CellKind::BigUint64ArrayKind) &&
          "Use ArrayStorage for BigInt64/BigUint64 typed array");
    }
#endif
  }

  // Swap elements at indices a and b.
  virtual ExecutionStatus swap(uint32_t a, uint32_t b) override {
    NoAllocScope noAlloc{runtime_};
    HermesValue tmp = getElement(a);
    setElement(a, getElement(b));
    setElement(b, tmp);
    return ExecutionStatus::RETURNED;
  }

  // Compare elements at index a and at index b.
  virtual CallResult<int> compare(uint32_t a, uint32_t b) override {
    GCScopeMarkerRAII gcMarker{gcScope_, gcMarker_};

    lv_.a = getElement(a);
    lv_.b = getElement(b);
    auto callRes = Callable::executeCall2(
        compareFn_,
        runtime_,
        Runtime::getUndefinedValue(),
        lv_.a.getHermesValue(),
        lv_.b.getHermesValue());
    if (callRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    lv_.callResult = std::move(*callRes);
    auto intRes = toNumber_RJS(runtime_, lv_.callResult);
    if (intRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    // Cannot return intRes's value directly because it can be NaN
    auto res = intRes->getNumber();
    return (res < 0) ? -1 : (res > 0 ? 1 : 0);
  }
};

namespace {

template <typename T>
int compareFloat(T va, T vb) {
  // NaN sorts after everything.
  bool aNaN = std::isnan(va);
  bool bNaN = std::isnan(vb);
  if (LLVM_UNLIKELY(aNaN)) {
    if (LLVM_UNLIKELY(bNaN))
      return 0;
    return 1;
  } else if (LLVM_UNLIKELY(bNaN)) {
    return -1;
  }
  // -0 < +0.
  if (LLVM_UNLIKELY(va == 0 && vb == 0))
    return std::signbit(vb) - std::signbit(va);
  return (va > vb) - (va < vb);
}

/// ::qsort comparator for typed array elements.
template <typename T, CellKind C>
int qsortComparator(const void *a, const void *b) {
  if constexpr (C == CellKind::Float16ArrayKind) {
    return compareFloat(
        float16ToDouble(*static_cast<const uint16_t *>(a)),
        float16ToDouble(*static_cast<const uint16_t *>(b)));
  } else if constexpr (std::is_floating_point_v<T>) {
    return compareFloat(*static_cast<const T *>(a), *static_cast<const T *>(b));
  } else {
    T va = *static_cast<const T *>(a);
    T vb = *static_cast<const T *>(b);
    return (va > vb) - (va < vb);
  }
}

/// Sort typed array elements directly using ::qsort.
void typedArraySortDirect(
    uint8_t *data,
    CellKind kind,
    JSTypedArrayBase::size_type len) {
  assert(data && "data must be non-null");
  switch (kind) {
#define TYPED_ARRAY(name, type)                            \
  case CellKind::name##ArrayKind:                          \
    ::qsort(                                               \
        data,                                              \
        len,                                               \
        sizeof(type),                                      \
        qsortComparator<type, CellKind::name##ArrayKind>); \
    break;
#include "hermes/VM/TypedArrays.def"
    default:
      llvm_unreachable("Unknown TypedArray kind");
  }
}

} // namespace

// ES7 22.2.3.23.1
CallResult<HermesValue> typedArrayPrototypeSetObject(
    Runtime &runtime,
    Handle<JSTypedArrayBase> self,
    Handle<> obj,
    double offset) {
  struct : public Locals {
    PinnedValue<JSObject> src;
    PinnedValue<> propValue;
    PinnedValue<> kValue;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  double targetLength = self->getLength();
  auto objRes = toObject(runtime, obj);
  if (objRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.src.castAndSetHermesValue<JSObject>(objRes.getValue());
  auto propRes = JSObject::getNamed_RJS(
      lv.src, runtime, Predefined::getSymbolID(Predefined::length));
  if (propRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.propValue = std::move(*propRes);
  auto intRes = toLength(runtime, lv.propValue);
  if (intRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t srcLength = intRes->getNumberAs<uint64_t>();
  if (srcLength + offset > targetLength) {
    return runtime.raiseRangeError(
        "The sum of the length of the given object "
        "and the offset cannot be greater than the length "
        "of this TypedArray");
  }
  // Read everything from the other array and write it into self starting from
  // offset.
  GCScope scope(runtime);
  uint64_t k = 0;
  auto marker = scope.createMarker();
  for (; k < srcLength; ++k) {
    if ((propRes = getIndexed_RJS(runtime, lv.src, k)) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.kValue = std::move(*propRes);
    if (JSObject::setOwnIndexed(self, runtime, offset++, lv.kValue) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    scope.flushToMarker(marker);
  }
  return HermesValue::encodeUndefinedValue();
}

// ES7 22.2.3.23.2
CallResult<HermesValue> typedArrayPrototypeSetTypedArray(
    Runtime &runtime,
    Handle<JSTypedArrayBase> self,
    Handle<JSTypedArrayBase> src,
    double offset) {
  if (!src->attached(runtime)) {
    return runtime.raiseTypeError(
        "The src TypedArray must be attached in order to use set()");
  }
  const JSTypedArrayBase::size_type srcLength = src->getLength();
  if (static_cast<double>(srcLength) + offset > self->getLength()) {
    return runtime.raiseRangeError(
        "The sum of the length of the given TypedArray "
        "and the offset cannot be greater than the length "
        "of this TypedArray");
  }
  // Since `src` is immutable, put the rest of the function into a continuation
  // to be called with a different `src` parameter.
  if (self->getBuffer(runtime)->getDataBlock() !=
      src->getBuffer(runtime)->getDataBlock()) {
    if (JSTypedArrayBase::setToCopyOfTypedArray(
            runtime, self, offset, src, 0, srcLength) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    return HermesValue::encodeUndefinedValue();
  }
  // 23. If SameValue(srcBuffer, targetBuffer) is true, then
  // a. Let srcBuffer be ? CloneArrayBuffer(targetBuffer, srcByteOffset,
  // %ArrayBuffer%).
  // If the two arrays have overlapping storage, make a copy of the source
  // array.
  // TODO: This could be implemented via a directional copy which either
  // copies forwards or backwards depending on how the regions overlap.
  auto possibleTA = src->allocate(runtime, srcLength);
  if (possibleTA == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto newSrc = possibleTA.getValue();
  // Create a handle directly without going through Locals since this is a
  // simple operation
  struct : public Locals {
    PinnedValue<JSArrayBuffer> srcBuffer;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.srcBuffer = src->getBuffer(runtime);
  if (JSTypedArrayBase::setToCopyOfBuffer(
          runtime,
          newSrc,
          0,
          lv.srcBuffer,
          src->getByteOffset(),
          src->getByteLength()) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // Now copy from newSrc into self.
  if (JSTypedArrayBase::setToCopyOfTypedArray(
          runtime, self, offset, newSrc, 0, srcLength) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return HermesValue::encodeUndefinedValue();
}

/// @}

} // namespace

/// @}

/// @name TypedArrayBase
/// @{

CallResult<HermesValue> typedArrayBaseConstructor(void *, Runtime &runtime) {
  return runtime.raiseTypeError(
      "TypedArray is abstract, it cannot be constructed");
}

/// @}

#define TYPED_ARRAY(name, type)                                            \
  CallResult<HermesValue> name##ArrayConstructor(void *ctx, Runtime &rt) { \
    NativeArgs args = rt.getCurrentFrame().getNativeArgs();                \
    return typedArrayConstructor<type, CellKind::name##ArrayKind>(         \
        ctx,                                                               \
        rt,                                                                \
        args,                                                              \
        &rt.name##ArrayConstructor,                                        \
        &rt.name##ArrayPrototype,                                          \
        &rt.class##name##Array);                                           \
  }
#include "hermes/VM/TypedArrays.def"
#undef TYPED_ARRAY

/// ES7 22.2.2.1
CallResult<HermesValue> typedArrayFrom(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<JSObject> arrayLike;
    PinnedValue<> propValue;
    PinnedValue<> mappedValue;
    PinnedValue<JSTypedArrayBase> targetObj;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto source = args.getArgHandle(0);
  // 1. Let C be the this value.
  if (!isConstructor(runtime, args.getThisArg())) {
    // 2. If IsConstructor(C) is false, throw a TypeError exception.
    return runtime.raiseTypeError(
        "Cannot invoke when the this is not a constructor");
  }
  auto C = Handle<Callable>::vmcast(runtime, args.getThisArg());
  // 3. If mapfn was supplied and mapfn is not undefined, then
  auto mapfn = Handle<Callable>::dyn_vmcast(args.getArgHandle(1));
  if (!mapfn) {
    // a. If IsCallable(mapfn) is false, throw a TypeError exception.
    if (args.getArgCount() >= 2 && !vmisa<Callable>(args.getArg(1))) {
      return runtime.raiseTypeError(
          "Second argument to TypedArray.from must be callable");
    }
    // b. Let mapping be true
  }
  // 4. Else, let mapping be false. (mapfn can act as a bool for mapping).
  // 5. If thisArg was supplied, let T be thisArg; else let T be undefined.
  auto T = args.getArgCount() >= 3 ? args.getArgHandle(2)
                                   : Runtime::getUndefinedValue();
  // 6. Let arrayLike be ? IterableToArrayLike(source).
  auto objRes = iterableToArrayLike(runtime, source);
  if (objRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.arrayLike.castAndSetHermesValue<JSObject>(objRes.getValue());
  // 7. Let len be ? ToLength(? Get(arrayLike, "length")).
  auto propRes = JSObject::getNamed_RJS(
      lv.arrayLike, runtime, Predefined::getSymbolID(Predefined::length));
  if (propRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.propValue = std::move(*propRes);
  auto intRes = toLength(runtime, lv.propValue);
  if (intRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t len = intRes.getValue().getNumberAs<uint64_t>();
  // 8. Let targetObj be ? TypedArrayCreate(C, len).
  MutableHandle<JSTypedArrayBase> targetObjHandle{lv.targetObj};
  if (typedArrayCreate(runtime, C, len, targetObjHandle) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 9. Let k be 0.
  // Note: typedArrayCreate validates that len fits in size_type.
  JSTypedArrayBase::size_type k = 0;
  // 10. Repeat, while k < len.
  for (; k < len; ++k) {
    GCScopeMarkerRAII marker{runtime};
    // a - b. Get the value of the property at k.
    if ((propRes = getIndexed_RJS(runtime, lv.arrayLike, k)) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    // c. If mapping is true, then
    if (mapfn) {
      // i. Let mappedValue be ? Call(mapfn, T, [kValue, k]).
      auto callRes = Callable::executeCall2(
          mapfn,
          runtime,
          T,
          propRes->get(),
          HermesValue::encodeTrustedNumberValue(k));
      if (callRes == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.mappedValue = std::move(*callRes);
    } else {
      lv.mappedValue = std::move(*propRes);
    }
    // NOTE: The returned value is either the same as the getComputed call, or
    // the call to the mapfn, so either way it is the correct value.
    // d. Else, let mappedValue be kValue (already done by initializer).
    // e. Perform ? Set(targetObj, Pk, mappedValue, true).
    if (JSObject::setOwnIndexed(lv.targetObj, runtime, k, lv.mappedValue) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    // f. Increase k by 1. (In the loop increment).
  }
  // 11. Return targetObj.
  return lv.targetObj.getHermesValue();
}

/// ES7 22.2.2.2
CallResult<HermesValue> typedArrayOf(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<> kValue;
    PinnedValue<JSTypedArrayBase> newObj;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 1. Let len be the actual number of arguments passed to this function.
  uint64_t len = args.getArgCount();
  // 2. Let items be the List of arguments passed to this function. (args is
  // items).
  // 3. Let C be the this value.
  if (!isConstructor(runtime, args.getThisArg())) {
    // 4. If IsConstructor(C) is false, throw a TypeError exception.
    return runtime.raiseTypeError(
        "Cannot invoke %TypedArray%.of when %TypedArray% is not a constructor "
        "function");
  }
  auto C = Handle<Callable>::vmcast(args.getThisHandle());
  // 5. Let newObj be ? TypedArrayCreate(C, len).
  MutableHandle<JSTypedArrayBase> newObjHandle{lv.newObj};
  if (typedArrayCreate(runtime, C, len, newObjHandle) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 6. Let k be 0.
  GCScope scope(runtime);
  auto marker = scope.createMarker();
  // 7. Repeat, while k < len.
  // Note: typedArrayCreate validates that len fits in size_type.
  for (JSTypedArrayBase::size_type k = 0; k < len; ++k) {
    // a. Let kValue be items[k].
    lv.kValue = args.getArg(k);
    // b. Let Pk be ! ToString(k).
    // c. Perform ? Set(newObj, Pk, kValue, true).
    if (JSObject::setOwnIndexed(lv.newObj, runtime, k, lv.kValue) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    // d. Increase k by 1. (In the loop increment).
    scope.flushToMarker(marker);
  }
  // Return newObj.
  return lv.newObj.getHermesValue();
}
/// @}

/// @name %JSTypedArray%.prototype
/// @{

/// ES16 23.2.3.2
CallResult<HermesValue> typedArrayPrototypeBuffer(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto self = args.dyncastThis<JSTypedArrayBase>();
  if (LLVM_UNLIKELY(!self)) {
    return runtime.raiseTypeError(
        "%TypedArray%.prototype.buffer called on a non-TypedArray");
  }
  return HermesValue::encodeObjectValue(self->getBuffer(runtime));
}

/// ES16 23.2.3.3
CallResult<HermesValue> typedArrayPrototypeByteLength(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto self = args.dyncastThis<JSTypedArrayBase>();
  if (LLVM_UNLIKELY(!self)) {
    return runtime.raiseTypeError(
        "%TypedArray%.prototype.byteLength called on a non-TypedArray");
  }
  return HermesValue::encodeTrustedNumberValue(
      self->attached(runtime) ? self->getByteLength() : 0);
}

/// ES16 23.2.3.4
CallResult<HermesValue> typedArrayPrototypeByteOffset(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto self = args.dyncastThis<JSTypedArrayBase>();
  if (LLVM_UNLIKELY(!self)) {
    return runtime.raiseTypeError(
        "%TypedArray%.prototype.byteOffset called on a non-TypedArray");
  }
  return HermesValue::encodeTrustedNumberValue(
      self->attached(runtime) && self->getLength() != 0 ? self->getByteOffset()
                                                        : 0);
}

/// ES6 23.2.3.1
CallResult<HermesValue> typedArrayPrototypeAt(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. Let O be the this value.
  // 2. Perform ? ValidateTypedArray(O).
  if (LLVM_UNLIKELY(
          JSTypedArrayBase::validateTypedArray(
              runtime, args.getThisHandle(), true) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  GCScope gcScope{runtime};

  auto O = args.vmcastThis<JSTypedArrayBase>();

  // 3. Let len be O.[[ArrayLength]].
  // The this object’s [[ArrayLength]] internal slot is accessed in place of
  // performing a [[Get]] of "length".
  double len = O->getLength();

  // 4. Let relativeIndex be ? ToIntegerOrInfinity(index).
  auto idx = args.getArgHandle(0);
  auto relativeIndexRes = toIntegerOrInfinity(runtime, idx);
  if (relativeIndexRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  const double relativeIndex = relativeIndexRes->getNumber();

  double k;
  // 5. If relativeIndex ≥ 0, then
  if (relativeIndex >= 0) {
    // a. Let k be relativeIndex.
    k = relativeIndex;
  } else {
    // 6. Else,
    // a. Let k be len + relativeIndex.
    k = len + relativeIndex;
  }

  // 7. If k < 0 or k ≥ len, return undefined.
  if (k < 0 || k >= len) {
    return HermesValue::encodeUndefinedValue();
  }

  // 8. Return ? Get(O, ! ToString(�(k))).
  // Since we know we have a TypedArray, we can directly call JSTypedArray::at
  // rather than getComputed_RJS like the spec mandates.
  auto *arr = vmcast<JSTypedArrayBase>(*O);
  if (LLVM_UNLIKELY(!arr->attached(runtime)))
    return runtime.raiseTypeError("Underlying ArrayBuffer detached");

  return JSTypedArrayBase::polyReadMayAlloc(
      arr, runtime, JSTypedArrayBase::size_type(k));
}

/// ES6 22.2.3.5
CallResult<HermesValue> typedArrayPrototypeCopyWithin(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (JSTypedArrayBase::validateTypedArray(
          runtime, args.getThisHandle(), true) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  GCScope gcScope{runtime};

  auto O = args.vmcastThis<JSTypedArrayBase>();

  // The this object’s [[ArrayLength]] internal slot is accessed in place of
  // performing a [[Get]] of "length".
  double len = O->getLength();

  // 5. Let relativeTarget be ToIntegerOrInfinity(target).
  // 6. ReturnIfAbrupt(relativeTarget).
  auto relativeTargetRes = toIntegerOrInfinity(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(relativeTargetRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double relativeTarget = relativeTargetRes->getNumber();

  // 7. If relativeTarget < 0, let to be max((len + relativeTarget),0); else let
  // to be min(relativeTarget, len).
  double to = convertNegativeBoundsRelativeToLength(relativeTarget, len);

  // 8. Let relativeStart be ToIntegerOrInfinity(start).
  // 9. ReturnIfAbrupt(relativeStart).
  auto relativeStartRes = toIntegerOrInfinity(runtime, args.getArgHandle(1));
  if (LLVM_UNLIKELY(relativeStartRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double relativeStart = relativeStartRes->getNumber();

  // 10. If relativeStart < 0, let from be max((len + relativeStart),0); else
  // let from be min(relativeStart, len).
  double from = convertNegativeBoundsRelativeToLength(relativeStart, len);

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
  double fin = convertNegativeBoundsRelativeToLength(relativeEnd, len);

  // 14. Let count be min(final-from, len-to).
  double count = std::min(fin - from, len - to);

  auto *baseArr = vmcast<JSTypedArrayBase>(*O);
  if (!baseArr->attached(runtime)) {
    return runtime.raiseTypeError(
        "Underlying ArrayBuffer detached after calling copyWithin");
  }

  // Get the byte width for this typed array
  const size_t elemSize = baseArr->getByteWidth();
  uint8_t *data = baseArr->data(runtime);
  // Use memmove to handle the overlapping regions correctly
  if (count > 0) {
    memmove(
        data + (static_cast<size_t>(to) * elemSize), // destination
        data + (static_cast<size_t>(from) * elemSize), // source
        static_cast<size_t>(count) * elemSize); // byte count
  }

  return O.getHermesValue();
}

// ES6 22.2.3.7 and 22.2.3.25 (also see Array.prototype.every/some)
CallResult<HermesValue> typedArrayPrototypeEverySome(
    void *ctx,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // NOTE: this was implemented as separate from Array.prototype.every to take
  // advantage of the known contiguous memory region.
  GCScope gcScope(runtime);
  auto every = static_cast<bool>(ctx);
  if (JSTypedArrayBase::validateTypedArray(runtime, args.getThisHandle()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  auto callbackfn = args.dyncastArg<Callable>(0);
  if (!callbackfn) {
    return runtime.raiseTypeError("callbackfn must be a Callable");
  }
  auto thisArg = args.getArgHandle(1);
  // Run the callback over every element.
  auto marker = gcScope.createMarker();
  for (JSTypedArrayBase::size_type i = 0; i < self->getLength(); ++i) {
    HermesValue val =
        JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, i);
    auto callRes = Callable::executeCall3(
        callbackfn,
        runtime,
        thisArg,
        val,
        HermesValue::encodeTrustedNumberValue(i),
        self.getHermesValue());
    if (callRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    gcScope.flushToMarker(marker);
    auto testResult = toBoolean(callRes->get());
    if (every && !testResult) {
      return HermesValue::encodeBoolValue(false);
    } else if (!every && testResult) {
      return HermesValue::encodeBoolValue(true);
    }
  }
  // If we're looking for every, then we finished without returning true.
  // If we're looking for some, then we finished without returning false.
  return HermesValue::encodeBoolValue(every);
}

// ES6 22.2.3.8
CallResult<HermesValue> typedArrayPrototypeFill(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<> value;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  if (JSTypedArrayBase::validateTypedArray(runtime, args.getThisHandle()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  const double len = self->getLength();
  CallResult<HermesValue> res = ExecutionStatus::EXCEPTION;
  switch (self->getKind()) {
    default:
      res = toNumber_RJS(runtime, args.getArgHandle(0));
      break;
    case CellKind::BigInt64ArrayKind:
    case CellKind::BigUint64ArrayKind:
      res = toBigInt_RJS(runtime, args.getArgHandle(0));
      break;
  }
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.value = res.getValue();
  res = toIntegerOrInfinity(runtime, args.getArgHandle(1));
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  const double relativeStart = res->getNumber();
  auto end = args.getArgHandle(2);
  if (!end->isUndefined()) {
    res = toIntegerOrInfinity(runtime, end);
    if (res == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  const double relativeEnd = end->isUndefined() ? len : res->getNumber();
  // At this point it is safe to convert to integers because the values will be
  // in the range [0, len], and len is an integer.
  const int64_t k = convertNegativeBoundsRelativeToLength(relativeStart, len);
  const int64_t last = convertNegativeBoundsRelativeToLength(relativeEnd, len);

  // 9. If IsDetachedBuffer(O.[[ViewedArrayBuffer]]) is true, throw a TypeError
  // exception.
  if (!self->attached(runtime)) {
    return runtime.raiseTypeError("Cannot fill a detached TypedArray");
  }

  if (k >= last) {
    // Early return to avoid the case of zero requested fill space.
    return self.getHermesValue();
  }

  if (JSObject::setOwnIndexed(self, runtime, k, lv.value) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto elementSize = self->getByteWidth();
  uint8_t *begin = self->data(runtime);
  // Fill with the same raw bytes as the first one.
  switch (elementSize) {
    case 1:
      std::fill(begin + k, begin + last, *(begin + k));
      break;
    case 2: {
      auto *src = reinterpret_cast<uint16_t *>(begin);
      std::fill(src + k, src + last, *(src + k));
      break;
    }
    case 4: {
      auto *src = reinterpret_cast<uint32_t *>(begin);
      std::fill(src + k, src + last, *(src + k));
      break;
    }
    case 8: {
      auto *src = reinterpret_cast<uint64_t *>(begin);
      std::fill(src + k, src + last, *(src + k));
      break;
    }
    default:
      llvm_unreachable("No element that is that wide");
  }
  return self.getHermesValue();
}

static CallResult<HermesValue>
typedFindHelper(void *ctx, bool reverse, Runtime &runtime, NativeArgs args) {
  struct : public Locals {
    PinnedValue<> val;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  bool index = static_cast<bool>(ctx);
  if (JSTypedArrayBase::validateTypedArray(runtime, args.getThisHandle()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  auto len = self->getLength();
  auto callbackfn = args.dyncastArg<Callable>(0);
  if (!callbackfn) {
    return runtime.raiseTypeError("callbackfn must be a Callable");
  }
  auto thisArg = args.getArgHandle(1);
  GCScope gcScope(runtime);
  auto marker = gcScope.createMarker();
  for (JSTypedArrayBase::size_type counter = 0; counter < len; counter++) {
    auto i = reverse ? (len - counter - 1) : counter;
    lv.val =
        JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, i);
    auto idx = HermesValue::encodeTrustedNumberValue(i);
    auto callRes = Callable::executeCall3(
        callbackfn,
        runtime,
        thisArg,
        lv.val.getHermesValue(),
        idx,
        self.getHermesValue());
    if (callRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    if (toBoolean(callRes->get())) {
      // Found one, return it.
      return index ? idx : lv.val.getHermesValue();
    }
    gcScope.flushToMarker(marker);
  }
  return index ? HermesValue::encodeTrustedNumberValue(-1)
               : HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue> typedArrayPrototypeFind(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return typedFindHelper(ctx, false, runtime, args);
}

CallResult<HermesValue> typedArrayPrototypeFindLast(
    void *ctx,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return typedFindHelper(ctx, true, runtime, args);
}

CallResult<HermesValue> typedArrayPrototypeForEach(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (JSTypedArrayBase::validateTypedArray(runtime, args.getThisHandle()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  auto len = self->getLength();
  auto callbackfn = args.dyncastArg<Callable>(0);
  if (!callbackfn) {
    return runtime.raiseTypeError("callbackfn must be a Callable");
  }
  auto thisArg = args.getArgHandle(1);
  GCScope gcScope(runtime);
  auto marker = gcScope.createMarker();
  for (JSTypedArrayBase::size_type i = 0; i < len; ++i) {
    HermesValue val =
        JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, i);
    if (Callable::executeCall3(
            callbackfn,
            runtime,
            thisArg,
            val,
            HermesValue::encodeTrustedNumberValue(i),
            self.getHermesValue()) == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    gcScope.flushToMarker(marker);
  }
  return HermesValue::encodeUndefinedValue();
}

namespace {

enum class IndexOfMode { includes, indexOf, lastIndexOf };

/// Encode a search result as a HermesValue.
/// For includes mode, returns a boolean. For indexOf/lastIndexOf, returns
/// the index (or -1 if not found).
HermesValue
indexOfResult(IndexOfMode mode, bool found = false, double index = -1) {
  if (mode == IndexOfMode::includes)
    return HermesValue::encodeBoolValue(found);
  return HermesValue::encodeTrustedNumberValue(index);
}

/// Dispatch to the correct SIMD search function based on UType.
/// Uses if constexpr to select the right function, avoiding macro
/// token-pasting.
template <typename UType>
int64_t
fastSearch(llvh::ArrayRef<UType> arr, size_t start, size_t end, UType target) {
  if constexpr (std::is_same_v<UType, uint8_t>) {
    return searchU8(arr, start, end, target);
  } else if constexpr (std::is_same_v<UType, uint16_t>) {
    return searchU16(arr, start, end, target);
  } else if constexpr (std::is_same_v<UType, uint32_t>) {
    return searchU32(arr, start, end, target);
  } else {
    static_assert(std::is_same_v<UType, uint64_t>, "unexpected unsigned type");
    return searchU64(arr, start, end, target);
  }
}

/// Dispatch to the correct reverse SIMD search function.
template <typename UType>
int64_t fastSearchReverse(
    llvh::ArrayRef<UType> arr,
    size_t start,
    size_t end,
    UType target) {
  if constexpr (std::is_same_v<UType, uint8_t>) {
    return searchReverseU8(arr, start, end, target);
  } else if constexpr (std::is_same_v<UType, uint16_t>) {
    return searchReverseU16(arr, start, end, target);
  } else if constexpr (std::is_same_v<UType, uint32_t>) {
    return searchReverseU32(arr, start, end, target);
  } else {
    static_assert(std::is_same_v<UType, uint64_t>, "unexpected unsigned type");
    return searchReverseU64(arr, start, end, target);
  }
}

/// Search an integer typed array for \p searchNum using SIMD.
/// Converts searchNum to element type T, checking that it is exactly
/// representable, then delegates to the appropriate SIMD search function.
template <typename T>
CallResult<HermesValue> tryFastIntSearch(
    T *typedData,
    double searchNum,
    size_t start,
    size_t end,
    bool isReverse,
    IndexOfMode mode) {
  // Integer arrays cannot contain NaN, so NaN never matches.
  if (std::isnan(searchNum))
    return indexOfResult(mode);
  // Check range before casting to avoid UB for out-of-range values.
  if (searchNum < static_cast<double>(std::numeric_limits<T>::min()) ||
      searchNum > static_cast<double>(std::numeric_limits<T>::max())) {
    return indexOfResult(mode);
  }
  T intVal = static_cast<T>(searchNum);
  if (static_cast<double>(intVal) != searchNum) {
    // searchNum is not exactly representable as T, so no match.
    return indexOfResult(mode);
  }

  // Reinterpret as the corresponding unsigned type for SIMD search.
  using UType = std::make_unsigned_t<T>;
  UType raw = static_cast<UType>(intVal);
  llvh::ArrayRef<UType> arr(reinterpret_cast<const UType *>(typedData), end);
  int64_t result = isReverse ? fastSearchReverse(arr, 0, start + 1, raw)
                             : fastSearch(arr, start, end, raw);
  if (result >= 0)
    return indexOfResult(mode, true, result);
  return indexOfResult(mode);
}

/// Search a floating-point typed array for \p searchNum using SIMD.
/// Handles NaN (includes vs indexOf), +0/-0, narrowing check,
/// and SIMD raw-bit search for normal values.
/// \p FType is float or double. UType is deduced as the same-size unsigned int.
template <typename FType>
CallResult<HermesValue> tryFastFloatSearch(
    uint8_t *data,
    double searchNum,
    size_t start,
    size_t end,
    bool isReverse,
    IndexOfMode mode) {
  using UType = std::conditional_t<sizeof(FType) == 4, uint32_t, uint64_t>;
  auto *fdata = reinterpret_cast<FType *>(data);
  if (std::isnan(searchNum)) {
    // indexOf/lastIndexOf: NaN !== NaN, so no match.
    if (mode != IndexOfMode::includes)
      return indexOfResult(mode);
    // includes: NaN includes NaN, scalar scan.
    for (size_t i = start; i < end; ++i) {
      if (std::isnan(fdata[i]))
        return indexOfResult(mode, true, i);
    }
    return indexOfResult(mode);
  }
  if (searchNum == 0) {
    // +0/-0 match each other in JS. Scan for both bit patterns.
    constexpr UType posZeroBits = 0;
    constexpr UType negZeroBits = UType(1) << (sizeof(UType) * 8 - 1);
    auto *rawData = reinterpret_cast<const UType *>(fdata);
    if (isReverse) {
      for (size_t i = start + 1; i-- > 0;) {
        UType bits = rawData[i];
        if (bits == posZeroBits || bits == negZeroBits)
          return indexOfResult(mode, true, i);
      }
    } else {
      for (size_t i = start; i < end; ++i) {
        UType bits = rawData[i];
        if (bits == posZeroBits || bits == negZeroBits)
          return indexOfResult(mode, true, i);
      }
    }
    return indexOfResult(mode);
  }
  // Convert to FType and verify round-trip (no-op for double).
  FType fval = static_cast<FType>(searchNum);
  if (static_cast<double>(fval) != searchNum)
    return indexOfResult(mode);
  UType raw;
  memcpy(&raw, &fval, sizeof(raw));
  llvh::ArrayRef<UType> arr(reinterpret_cast<const UType *>(fdata), end);
  int64_t result = isReverse ? fastSearchReverse(arr, 0, start + 1, raw)
                             : fastSearch(arr, start, end, raw);
  if (result >= 0)
    return indexOfResult(mode, true, result);
  return indexOfResult(mode);
}

} // namespace

CallResult<HermesValue> typedArrayPrototypeIndexOf(
    void *ctx,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  const auto indexOfMode = *reinterpret_cast<const IndexOfMode *>(&ctx);
  if (JSTypedArrayBase::validateTypedArray(runtime, args.getThisHandle()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  double len = self->getLength();
  if (len == 0) {
    return indexOfResult(indexOfMode);
  }
  auto searchElement = args.getArgHandle(0);
  if (indexOfMode != IndexOfMode::includes && !searchElement->isNumber() &&
      !searchElement->isBigInt()) {
    // For "indexOf"/"lastIndexOf", if it's not a number, nothing will match.
    // However, for "includes", ES2026 23.2.3.16 step 11 uses `SameValueZero`
    // for comparison. This means that "undefined" can match if the TypedArray
    // becomes detached while in the loop below.
    return indexOfResult(indexOfMode);
  }
  double fromIndex = 0;
  if (args.getArgCount() < 2) {
    // Zero default for forward, end default for backward.
    if (indexOfMode == IndexOfMode::lastIndexOf) {
      fromIndex = len - 1;
    }
  } else {
    auto res = toIntegerOrInfinity(runtime, args.getArgHandle(1));
    if (res == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    fromIndex = res->getNumber();
  }
  // Negative zero case.
  if (fromIndex == 0) {
    fromIndex = 0;
  }
  double k = 0;
  if (indexOfMode == IndexOfMode::lastIndexOf) {
    k = fromIndex >= 0 ? std::min(fromIndex, len - 1) : len + fromIndex;
  } else {
    k = fromIndex >= 0 ? fromIndex : std::max(len + fromIndex, 0.0);
  }
  auto delta = indexOfMode == IndexOfMode::lastIndexOf ? -1 : 1;
  auto inRange = [indexOfMode](double k, double len) {
    if (indexOfMode == IndexOfMode::lastIndexOf) {
      return k >= 0;
    } else {
      return k < len;
    }
  };
  // SIMD fast path for numeric typed arrays (not BigInt).
  // Typed arrays have contiguous raw buffers, so we can search directly.
  // Check that k is in range before entering the fast path (k can be
  // out of range when fromIndex is extreme).
  // Also check that the buffer is still attached, since toIntegerOrInfinity
  // above can execute user code (e.g. valueOf) that detaches the buffer.
  if (searchElement->isNumber() && inRange(k, len) && self->attached(runtime)) {
    const CellKind kind = self->getKind();
    double searchNum = searchElement->getNumber();
    NoAllocScope noAllocs{runtime};
    uint8_t *data = self->data(runtime);
    size_t start = static_cast<size_t>(k);
    size_t end = static_cast<size_t>(len);
    bool isReverse = (indexOfMode == IndexOfMode::lastIndexOf);

    switch (kind) {
      case CellKind::Int8ArrayKind:
        return tryFastIntSearch(
            reinterpret_cast<int8_t *>(data),
            searchNum,
            start,
            end,
            isReverse,
            indexOfMode);
      case CellKind::Uint8ArrayKind:
      case CellKind::Uint8ClampedArrayKind:
        return tryFastIntSearch(
            reinterpret_cast<uint8_t *>(data),
            searchNum,
            start,
            end,
            isReverse,
            indexOfMode);
      case CellKind::Int16ArrayKind:
        return tryFastIntSearch(
            reinterpret_cast<int16_t *>(data),
            searchNum,
            start,
            end,
            isReverse,
            indexOfMode);
      case CellKind::Uint16ArrayKind:
        return tryFastIntSearch(
            reinterpret_cast<uint16_t *>(data),
            searchNum,
            start,
            end,
            isReverse,
            indexOfMode);
      case CellKind::Int32ArrayKind:
        return tryFastIntSearch(
            reinterpret_cast<int32_t *>(data),
            searchNum,
            start,
            end,
            isReverse,
            indexOfMode);
      case CellKind::Uint32ArrayKind:
        return tryFastIntSearch(
            reinterpret_cast<uint32_t *>(data),
            searchNum,
            start,
            end,
            isReverse,
            indexOfMode);
      case CellKind::Float32ArrayKind:
        return tryFastFloatSearch<float>(
            data, searchNum, start, end, isReverse, indexOfMode);
      case CellKind::Float64ArrayKind:
        return tryFastFloatSearch<double>(
            data, searchNum, start, end, isReverse, indexOfMode);
      default:
        // BigInt typed arrays: fall through to slow path.
        break;
    }
  }

  for (; inRange(k, len); k += delta) {
    HermesValue curr =
        JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, k);
    NoAllocScope noAllocs{runtime};

    bool comp = indexOfMode == IndexOfMode::includes
        ? isSameValueZero(curr, *searchElement)
        : strictEqualityTest(curr, *searchElement);
    if (comp) {
      return indexOfResult(indexOfMode, true, k);
    }
  }
  return indexOfResult(indexOfMode);
}

CallResult<HermesValue> typedArrayPrototypeIterator(
    void *ctx,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  IterationKind kind = *reinterpret_cast<IterationKind *>(&ctx);
  assert(
      kind <= IterationKind::NumKinds &&
      "typeArrayPrototypeIterator with wrong kind");
  if (JSTypedArrayBase::validateTypedArray(runtime, args.getThisHandle()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  return JSArrayIterator::create(runtime, self, kind).getHermesValue();
}

CallResult<HermesValue> typedArrayPrototypeMapFilter(
    void *ctx,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};

  // Whether this call is "map" or "filter".
  bool map = static_cast<bool>(ctx);
  if (JSTypedArrayBase::validateTypedArray(runtime, args.getThisHandle()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  JSTypedArrayBase::size_type len = self->getLength();
  auto callbackfn = args.dyncastArg<Callable>(0);
  if (!callbackfn) {
    return runtime.raiseTypeError("callbackfn must be a Callable");
  }
  auto thisArg = args.getArgHandle(1);
  struct : public Locals {
    PinnedValue<JSArray> values;
    PinnedValue<> storage;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Can't use a vector since this could store an unbounded number of handles.
  auto arrRes = JSArray::create(runtime, len, 0);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.values = std::move(*arrRes);
  JSTypedArrayBase::size_type insert = 0;
  CallResult<HermesValue> res{ExecutionStatus::EXCEPTION};
  if (map) {
    if ((res = mapFilterLoop<true>(
             runtime, self, callbackfn, thisArg, lv.values, insert, len)) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
  } else {
    if ((res = mapFilterLoop<false>(
             runtime, self, callbackfn, thisArg, lv.values, insert, len)) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  insert = res->getNumberAs<JSTypedArrayBase::size_type>();
  // Now create a new TypedArray of the same kind and fill it with the values.
  auto result = JSTypedArrayBase::allocateSpecies(runtime, self, insert);
  if (result == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto TA = result.getValue();
  auto marker = gcScope.createMarker();
  for (JSTypedArrayBase::size_type i = 0; i < insert; ++i) {
    lv.storage = lv.values->at(runtime, i).unboxToHV(runtime);
    if (JSObject::setOwnIndexed(TA, runtime, i, lv.storage) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    gcScope.flushToMarker(marker);
  }
  return TA.getHermesValue();
}

/// ES6 22.2.3.17
CallResult<HermesValue> typedArrayPrototypeLength(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (JSTypedArrayBase::validateTypedArray(
          runtime, args.getThisHandle(), false) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  return HermesValue::encodeTrustedNumberValue(
      self->attached(runtime) ? self->getLength() : 0);
}

CallResult<HermesValue> typedArrayPrototypeJoin(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // NOTE: there are probably some optimizations that can be made here due to
  // operating on only numbers in typed arrays.
  if (JSTypedArrayBase::validateTypedArray(runtime, args.getThisHandle()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  auto len = self->getLength();
  struct : public Locals {
    PinnedValue<> separator;
    PinnedValue<StringPrimitive> sep;
    PinnedValue<JSArray> strings;
    PinnedValue<> elem;
    PinnedValue<StringPrimitive> element;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.separator = args.getArg(0).isUndefined()
      ? HermesValue::encodeStringValue(
            runtime.getPredefinedString(Predefined::comma))
      : args.getArg(0);
  auto res = toString_RJS(runtime, lv.separator);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.sep = PseudoHandle<StringPrimitive>::vmcast(std::move(*res));
  if (len == 0) {
    // Quick exit for empty arrays to avoid allocations.
    // NOTE: this needs to come after the `toString` call on the separator
    // according to the spec.
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::emptyString));
  }

  auto sepLength = lv.sep->getStringLength();
  if (len > std::numeric_limits<uint32_t>::max() ||
      sepLength > (double)StringPrimitive::MAX_STRING_LENGTH / len) {
    // Check for overflow.
    return runtime.raiseRangeError(
        "String.prototype.repeat result exceeds limit");
  }

  // Final size of the resultant string.
  // Its safe to multiply as overflow check is done above
  SafeUInt32 size(sepLength * (len - 1));
  // Whether the separator, if used, and every element string are ASCII.
  bool allASCII = len < 2 || lv.sep->isASCII();

  // Storage for the strings for each element.
  auto arrRes = JSArray::create(runtime, len, 0);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.strings = std::move(*arrRes);

  // Call toString on all the elements of the array.
  {
    // Make sure to drop the Handle early, it's not needed outside of the loop.
    for (decltype(len) i = 0; i < len; ++i) {
      GCScope gcScope(runtime);
      lv.elem =
          JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, i);

      if (LLVM_UNLIKELY(lv.elem->isUndefined())) {
        lv.elem = HermesValue::encodeStringValue(
            runtime.getPredefinedString(Predefined::emptyString));
      } else {
        auto res2 = toString_RJS(runtime, lv.elem);
        if (LLVM_UNLIKELY(res2 == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        lv.elem = std::move(*res2);
      }
      auto stringPtr = vmcast<StringPrimitive>(lv.elem.getHermesValue());
      size.add(stringPtr->getStringLength());
      allASCII &= stringPtr->isASCII();
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(
                  lv.strings,
                  runtime,
                  i,
                  Handle<StringPrimitive>::vmcast(&lv.elem)) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
    }
  }

  // Allocate the complete result.
  auto builder = StringBuilder::createStringBuilder(runtime, size, allASCII);
  if (builder == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.element =
      createPseudoHandle(lv.strings->at(runtime, 0).getString(runtime));
  builder->appendStringPrim(lv.element);
  // Copy the strings.
  for (decltype(len) i = 1; i < len; ++i) {
    builder->appendStringPrim(lv.sep);
    lv.element =
        createPseudoHandle(lv.strings->at(runtime, i).getString(runtime));
    builder->appendStringPrim(lv.element);
  }
  return HermesValue::encodeStringValue(*builder->getStringPrimitive());
}

CallResult<HermesValue> typedArrayPrototypeReduce(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // Whether this call is "reduce" or "reduceRight".
  bool right = static_cast<bool>(ctx);
  if (JSTypedArrayBase::validateTypedArray(runtime, args.getThisHandle()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  double len = self->getLength();
  auto callbackfn = args.dyncastArg<Callable>(0);
  if (!callbackfn) {
    return runtime.raiseTypeError("callbackfn must be a Callable");
  }
  const bool calledWithInitialValue = args.getArgCount() >= 2;
  if (len == 0 && !calledWithInitialValue) {
    return runtime.raiseTypeError(
        "reduce needs to provide an initial value for an empty TypedArray");
  }
  struct : public Locals {
    PinnedValue<> accumulator;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // If the intial value is not provided, it must be the first value from the
  // array.
  if (calledWithInitialValue) {
    lv.accumulator = args.getArg(1);
  } else {
    lv.accumulator = JSObject::getOwnIndexed(
        createPseudoHandle(self.get()), runtime, right ? len - 1 : 0);
  }

  auto inRange = [right](double i, double len) {
    return right ? i >= 0 : i < len;
  };
  double i = right ? len - 1 : 0;
  // Move it up one if there was not an initial value specified.
  if (!calledWithInitialValue) {
    i += right ? -1 : 1;
  }

  Handle<> undefinedThis = Runtime::getUndefinedValue();
  GCScope scope(runtime);
  auto marker = scope.createMarker();
  for (; inRange(i, len); i += right ? -1 : 1) {
    HermesValue val =
        JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, i);
    auto callRes = Callable::executeCall4(
        callbackfn,
        runtime,
        undefinedThis,
        lv.accumulator.getHermesValue(),
        val,
        HermesValue::encodeTrustedNumberValue(i),
        self.getHermesValue());
    if (callRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.accumulator = std::move(*callRes);
    scope.flushToMarker(marker);
  }
  return lv.accumulator.getHermesValue();
}

// ES7 22.2.3.22
CallResult<HermesValue> typedArrayPrototypeReverse(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<> lowerHandle;
    PinnedValue<> upperHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  if (JSTypedArrayBase::validateTypedArray(runtime, args.getThisHandle()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  const JSTypedArrayBase::size_type len = self->getLength();
  const JSTypedArrayBase::size_type middle = len / 2;
  for (JSTypedArrayBase::size_type lower = 0; lower != middle; ++lower) {
    auto upper = len - lower - 1;
    lv.lowerHandle =
        JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, lower);
    lv.upperHandle =
        JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, upper);
    if (JSObject::setOwnIndexed(self, runtime, lower, lv.upperHandle) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    if (JSObject::setOwnIndexed(self, runtime, upper, lv.lowerHandle) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return self.getHermesValue();
}

/// ES7 22.2.3.26
CallResult<HermesValue> typedArrayPrototypeSort(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  // Null if not a callable compareFn.
  auto compareFn = Handle<Callable>::dyn_vmcast(args.getArgHandle(0));
  if (!args.getArg(0).isUndefined() && !compareFn) {
    return runtime.raiseTypeError("TypedArray sort argument must be callable");
  }

  if (JSTypedArrayBase::validateTypedArray(runtime, args.getThisHandle()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  const JSTypedArrayBase::size_type len = self->getLength();

  if (compareFn) {
    // If this array is BigUint64 or BigInt64 typed array, we copy the source
    // to an ArrayStorage. Otherwise, we copy the source to another TypedArray.
    // The reason is that to encode BigUint64/BigInt64 value into a HermesValue,
    // we need to allocate BigIntPrimitive for every compareFn call, which would
    // be very expensive.
    if (self->getKind() == CellKind::BigUint64ArrayKind ||
        self->getKind() == CellKind::BigInt64ArrayKind) {
      struct : public Locals {
        PinnedValue<ArrayStorage> arrStorage;
        PinnedValue<> value;
      } lv;
      LocalsRAII lraii(runtime, &lv);

      auto itemsRes = ArrayStorage::create(runtime, len, 0);
      if (LLVM_UNLIKELY(itemsRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.arrStorage.castAndSetHermesValue<ArrayStorage>(*itemsRes);

      // ES16 23.1.3.30.1 SortIndexedProperties, we need to copy the values into
      // an internal list.
      for (JSTypedArrayBase::size_type i = 0; i < len; ++i) {
        lv.value = JSTypedArrayBase::polyReadMayAlloc(self.get(), runtime, i);
        lv.arrStorage->pushWithinCapacity(runtime, *lv.value);
      }
      TypedArraySortModel sm(runtime, lv.arrStorage, compareFn);
      if (LLVM_UNLIKELY(quickSort(&sm, 0, len) == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;

      if (self->attached(runtime)) {
        for (JSTypedArrayBase::size_type i = 0; i < len; ++i) {
          HermesValue value = lv.arrStorage->at(i);
          JSTypedArrayBase::polyWriteNoAlloc(*self, runtime, i, value);
        }
      }
    } else {
      struct : public Locals {
        PinnedValue<JSTypedArrayBase> newTypedArray;
      } lv;
      LocalsRAII lraii(runtime, &lv);

      auto newTypedArrayRes = self->allocate(runtime, len);
      if (LLVM_UNLIKELY(newTypedArrayRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.newTypedArray = *newTypedArrayRes;
      // This should just call setToCopyOfBytes() and should not throw
      // exception.
      [[maybe_unused]] auto status = JSTypedArrayBase::setToCopyOfTypedArray(
          runtime, lv.newTypedArray, 0, self, 0, len);
      assert(status == ExecutionStatus::RETURNED);
      TypedArraySortModel sm(runtime, lv.newTypedArray, compareFn);
      if (LLVM_UNLIKELY(quickSort(&sm, 0, len) == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      if (self->attached(runtime)) {
        status = JSTypedArrayBase::setToCopyOfTypedArray(
            runtime, self, 0, lv.newTypedArray, 0, len);
        assert(status == ExecutionStatus::RETURNED);
      }
    }
  } else if (len > 0) {
    typedArraySortDirect(self->data(runtime), self->getKind(), len);
  }
  return self.getHermesValue();
}

// ES7 22.2.3.23
CallResult<HermesValue> typedArrayPrototypeSet(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<> offset;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  if (JSTypedArrayBase::validateTypedArray(
          runtime, args.getThisHandle(), false) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  // Default to zero if unspecified.
  lv.offset = args.getArgCount() >= 2
      ? args.getArg(1)
      : HermesValue::encodeTrustedNumberValue(0);
  auto res = toIntegerOrInfinity(runtime, lv.offset);
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  double targetOffset = res->getNumber();
  // Offset check must come before the attached check.
  if (targetOffset < 0) {
    return runtime.raiseRangeError("Offset must not be negative if supplied");
  }
  if (!self->attached(runtime)) {
    return runtime.raiseTypeError(
        "TypedArray.prototype.set called on a detached TypedArray");
  }
  // Detect the type of the first argument to determine which version to call.
  auto arr = args.getArgHandle(0);
  if (auto typedarr = Handle<JSTypedArrayBase>::dyn_vmcast(arr)) {
    return typedArrayPrototypeSetTypedArray(
        runtime, self, typedarr, targetOffset);
  } else {
    return typedArrayPrototypeSetObject(runtime, self, arr, targetOffset);
  }
}

// ES7 22.2.3.24
CallResult<HermesValue> typedArrayPrototypeSlice(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (JSTypedArrayBase::validateTypedArray(runtime, args.getThisHandle()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  double len = self->getLength();
  auto res = toIntegerOrInfinity(runtime, args.getArgHandle(0));
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto relativeStart = res->getNumber();
  double relativeEnd = 0;
  if (args.getArg(1).isUndefined()) {
    relativeEnd = len;
  } else {
    res = toIntegerOrInfinity(runtime, args.getArgHandle(1));
    if (res == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    relativeEnd = res->getNumber();
  }
  double k = convertNegativeBoundsRelativeToLength(relativeStart, len);
  double last = convertNegativeBoundsRelativeToLength(relativeEnd, len);
  double count = std::max(last - k, 0.0);
  auto status = JSTypedArrayBase::allocateSpecies(runtime, self, count);
  if (status == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  if (!self->attached(runtime)) {
    // 15b. If IsDetachedBuffer(srcBuffer) is true, throw a TypeError exception.
    return runtime.raiseTypeError(
        "Detached the buffer in the species constructor");
  }
  auto A = status.getValue();
  if (count > 0) {
    JSTypedArrayBase::setToCopyOfTypedArray(runtime, A, 0, self, k, count);
  }
  return A.getHermesValue();
}

// ES7 22.2.3.27
CallResult<HermesValue> typedArrayPrototypeSubarray(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (JSTypedArrayBase::validateTypedArray(
          runtime, args.getThisHandle(), false) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  double srcLength = self->getLength();
  auto res = toIntegerOrInfinity(runtime, args.getArgHandle(0));
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  double relativeBegin = res->getNumber();
  double relativeEnd = srcLength;
  if (!args.getArg(1).isUndefined()) {
    res = toIntegerOrInfinity(runtime, args.getArgHandle(1));
    if (res == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    relativeEnd = res->getNumber();
  }
  double beginIndex =
      convertNegativeBoundsRelativeToLength(relativeBegin, srcLength);
  double endIndex =
      convertNegativeBoundsRelativeToLength(relativeEnd, srcLength);
  double newLength = std::max(endIndex - beginIndex, 0.0);
  auto result = JSTypedArrayBase::allocateToSameBuffer(
      runtime, self, beginIndex, beginIndex + newLength);
  if (result == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return result.getValue().getHermesValue();
}

CallResult<HermesValue> typedArrayPrototypeSymbolToStringTag(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto O = args.dyncastThis<JSObject>();
  if (!O) {
    return HermesValue::encodeUndefinedValue();
  }

  // 4. Let name be the value of O’s [[TypedArrayName]] internal slot.
#define TYPED_ARRAY(name, type)                                              \
  if (vmisa<JSTypedArray<type, CellKind::name##ArrayKind>>(*O)) {            \
    return HermesValue::encodeStringValue(runtime.getStringPrimFromSymbolID( \
        JSTypedArray<type, CellKind::name##ArrayKind>::getName(runtime)));   \
  }
#include "hermes/VM/TypedArrays.def"

  // 3. If O does not have a [[TypedArrayName]] internal slot, return undefined.
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue> typedArrayPrototypeToLocaleString(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope(runtime);
  if (JSTypedArrayBase::validateTypedArray(runtime, args.getThisHandle()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<JSTypedArrayBase> self = args.vmcastThis<JSTypedArrayBase>();

  auto emptyString = runtime.getPredefinedStringHandle(Predefined::emptyString);

  const JSTypedArrayBase::size_type len = self->getLength();
  if (len == 0) {
    return emptyString.getHermesValue();
  }

  // TODO: Get a list-separator String for the host environment's locale.
  // Use a comma as a separator for now, as JSC does.
  auto separator = createASCIIRef(",");

  // Final size of the result string. Initialize to account for the separators.
  SafeUInt32 size(len - 1);
  // Whether every element string is ASCII; the separator is.
  bool allASCII = true;

  struct : public Locals {
    PinnedValue<JSArray> strings;
    PinnedValue<> storage;
    PinnedValue<JSObject> elementObj;
    PinnedValue<> funcValue;
    PinnedValue<StringPrimitive> elementStr;
    PinnedValue<StringPrimitive> element;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Array to store each of the strings of the elements.
  auto arrRes = JSArray::create(runtime, len, len);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.strings = std::move(*arrRes);

  auto marker = gcScope.createMarker();
  for (JSTypedArrayBase::size_type i = 0; i < len; ++i) {
    lv.storage =
        JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, i);
    if (lv.storage.getHermesValue().isUndefined() ||
        lv.storage.getHermesValue().isNull()) {
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(lv.strings, runtime, i, emptyString) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      continue;
    }

    auto objRes = toObject(runtime, lv.storage);
    if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.elementObj.castAndSetHermesValue<JSObject>(objRes.getValue());

    // Retrieve the toLocaleString function.
    auto propRes = JSObject::getNamed_RJS(
        lv.elementObj,
        runtime,
        Predefined::getSymbolID(Predefined::toLocaleString));
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.funcValue = std::move(*propRes);
    if (auto func = Handle<Callable>::dyn_vmcast(Handle<>{lv.funcValue})) {
#ifdef HERMES_ENABLE_INTL
      auto callRes = Callable::executeCall2(
          func, runtime, lv.elementObj, args.getArg(0), args.getArg(1));
#else
      auto callRes = Callable::executeCall0(func, runtime, lv.elementObj);
#endif
      if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.funcValue = std::move(*callRes);
      auto strRes = toString_RJS(runtime, Handle<>::vmcast(&lv.funcValue));
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.elementStr = std::move(*strRes);
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(lv.strings, runtime, i, lv.elementStr) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      size.add(lv.elementStr->getStringLength());
      allASCII &= lv.elementStr->isASCII();
    } else {
      return runtime.raiseTypeError("toLocaleString() not callable");
    }
    gcScope.flushToMarker(marker);
  }

  auto builder = StringBuilder::createStringBuilder(runtime, size, allASCII);
  if (builder == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.element =
      createPseudoHandle(lv.strings->at(runtime, 0).getString(runtime));
  builder->appendStringPrim(lv.element);

  for (uint32_t i = 1; i < len; ++i) {
    // Every element after the first needs a separator before it.
    builder->appendASCIIRef(separator);
    lv.element = lv.strings->at(runtime, i).getString(runtime);
    builder->appendStringPrim(lv.element);
  }
  return HermesValue::encodeStringValue(*builder->getStringPrimitive());
}

//===----------------------------------------------------------------------===//
/// Uint8Array Base64/Hex Methods
/// TC39 "Uint8Array to/from Base64 and Hex" proposal
//===----------------------------------------------------------------------===//

namespace {

/// Result of a hex decode operation.
struct HexDecodeResult {
  uint32_t read;
  llvh::SmallVector<uint8_t, 64> bytes;
  bool error;

  HexDecodeResult() : read(0), error(false) {}
};

/// Decode a hex digit character to its 4-bit value. Returns -1 if invalid.
int decodeHexDigit(char16_t c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

/// Decode a hex string. Each pair of hex digits produces one byte.
/// \p input the input string view.
/// \p maxLength maximum number of bytes to decode.
HexDecodeResult fromHexImpl(StringView input, uint32_t maxLength) {
  HexDecodeResult result;
  uint32_t inputLen = input.length();

  for (uint32_t i = 0; i + 1 < inputLen && result.bytes.size() < maxLength;
       i += 2) {
    int hi = decodeHexDigit(input[i]);
    int lo = decodeHexDigit(input[i + 1]);
    if (hi < 0 || lo < 0) {
      result.error = true;
      result.read = i;
      return result;
    }
    result.bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
    result.read = i + 2;
  }

  return result;
}

/// Parse the base64 options object.
/// Returns the alphabet (useBase64url) and lastChunkHandling via output params.
/// Returns ExecutionStatus::EXCEPTION on error.
ExecutionStatus parseBase64Options(
    Runtime &runtime,
    Handle<> options,
    bool &useBase64url,
    LastChunkHandling &lastChunkHandling,
    bool &omitPadding) {
  struct : public Locals {
    PinnedValue<> strVal;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  useBase64url = false;
  lastChunkHandling = LastChunkHandling::Loose;
  omitPadding = false;

  if (options->isUndefined())
    return ExecutionStatus::RETURNED;

  // GetOptionsObject: throw TypeError if not an object.
  if (!vmisa<JSObject>(options.get())) {
    return runtime.raiseTypeError("options must be an object");
  }
  auto obj = Handle<JSObject>::vmcast(options);

  // Get 'alphabet' property.
  {
    auto propRes = JSObject::getNamed_RJS(
        obj, runtime, Predefined::getSymbolID(Predefined::alphabet));
    if (propRes == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    lv.strVal = std::move(*propRes);
    if (!lv.strVal->isUndefined()) {
      if (!lv.strVal->isString()) {
        return runtime.raiseTypeError("alphabet must be a string");
      }
      auto alphaStr = Handle<StringPrimitive>::vmcast(&lv.strVal);
      auto alphaView = StringPrimitive::createStringView(runtime, alphaStr);
      auto base64Ref = createASCIIRef("base64");
      auto base64urlRef = createASCIIRef("base64url");
      if (alphaView.equals(base64urlRef)) {
        useBase64url = true;
      } else if (!alphaView.equals(base64Ref)) {
        return runtime.raiseTypeError(
            "alphabet must be either \"base64\" or \"base64url\"");
      }
    }
  }

  // Get 'lastChunkHandling' property.
  {
    auto propRes = JSObject::getNamed_RJS(
        obj, runtime, Predefined::getSymbolID(Predefined::lastChunkHandling));
    if (propRes == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    lv.strVal = std::move(*propRes);
    if (!lv.strVal->isUndefined()) {
      if (!lv.strVal->isString()) {
        return runtime.raiseTypeError("lastChunkHandling must be a string");
      }
      auto lchStr = Handle<StringPrimitive>::vmcast(&lv.strVal);
      auto lchView = StringPrimitive::createStringView(runtime, lchStr);
      auto looseRef = createASCIIRef("loose");
      auto strictRef = createASCIIRef("strict");
      auto sbpRef = createASCIIRef("stop-before-partial");
      if (lchView.equals(strictRef)) {
        lastChunkHandling = LastChunkHandling::Strict;
      } else if (lchView.equals(sbpRef)) {
        lastChunkHandling = LastChunkHandling::StopBeforePartial;
      } else if (!lchView.equals(looseRef)) {
        return runtime.raiseTypeError(
            "lastChunkHandling must be \"loose\", \"strict\", or \"stop-before-partial\"");
      }
    }
  }

  // Get 'omitPadding' property.
  {
    auto propRes = JSObject::getNamed_RJS(
        obj, runtime, Predefined::getSymbolID(Predefined::omitPadding));
    if (propRes == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;
    omitPadding = toBoolean(propRes->get());
  }

  return ExecutionStatus::RETURNED;
}

/// Validate that 'this' is a Uint8Array per spec Section 7.
/// Only checks [[TypedArrayName]], not detachment.
CallResult<Handle<JSTypedArrayBase>> validateUint8Array(
    Runtime &runtime,
    Handle<> thisVal) {
  auto *ta = dyn_vmcast<JSTypedArrayBase>(thisVal.get());
  if (!ta || ta->getKind() != CellKind::Uint8ArrayKind) {
    return runtime.raiseTypeError("'this' is not a Uint8Array");
  }
  return Handle<JSTypedArrayBase>::vmcast(thisVal);
}

/// Create a plain object { read: readVal, written: writtenVal }.
CallResult<HermesValue> createReadWrittenResult(
    Runtime &runtime,
    uint32_t readVal,
    uint32_t writtenVal) {
  struct : public Locals {
    PinnedValue<JSObject> resultObj;
    PinnedValue<> tmpVal;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto objRes = JSObject::create(runtime);
  lv.resultObj = objRes.get();

  lv.tmpVal = HermesValue::encodeTrustedNumberValue(readVal);
  auto putRes = JSObject::putNamed_RJS(
      lv.resultObj,
      runtime,
      Predefined::getSymbolID(Predefined::read),
      lv.tmpVal);
  if (putRes == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;

  lv.tmpVal = HermesValue::encodeTrustedNumberValue(writtenVal);
  putRes = JSObject::putNamed_RJS(
      lv.resultObj,
      runtime,
      Predefined::getSymbolID(Predefined::written),
      lv.tmpVal);
  if (putRes == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;

  return lv.resultObj.getHermesValue();
}

/// Uint8Array.prototype.toBase64([options])
CallResult<HermesValue> uint8ArrayPrototypeToBase64(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};

  auto selfRes = validateUint8Array(runtime, args.getThisHandle());
  if (selfRes == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  auto self = *selfRes;

  bool useBase64url = false;
  LastChunkHandling lch = LastChunkHandling::Loose;
  bool omitPadding = false;
  if (parseBase64Options(
          runtime, args.getArgHandle(0), useBase64url, lch, omitPadding) ==
      ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;

  // Re-check attachment since parseBase64Options runs user-observable code.
  if (!self->attached(runtime)) {
    return runtime.raiseTypeError("Uint8Array is detached");
  }

  auto len = self->getLength();
  auto *data = self->data(runtime);

  // Calculate output length using uint64_t to avoid overflow.
  uint64_t fullChunks = len / 3;
  uint32_t remainder = len % 3;
  uint64_t outLen;
  if (omitPadding) {
    outLen = fullChunks * 4 + (remainder == 0 ? 0 : remainder + 1);
  } else {
    outLen = remainder == 0 ? fullChunks * 4 : (fullChunks + 1) * 4;
  }
  if (outLen > StringPrimitive::MAX_STRING_LENGTH) {
    return runtime.raiseRangeError(
        "base64 output exceeds maximum string length");
  }

  SafeUInt32 safeOutLen{static_cast<uint32_t>(outLen)};
  auto builder = StringBuilder::createStringBuilder(runtime, safeOutLen, true);
  if (builder == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;

  base64Encode(
      llvh::ArrayRef<uint8_t>(data, len), *builder, useBase64url, omitPadding);

  return builder->getStringPrimitive().getHermesValue();
}

/// Uint8Array.prototype.toHex()
CallResult<HermesValue> uint8ArrayPrototypeToHex(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  auto selfRes = validateUint8Array(runtime, args.getThisHandle());
  if (selfRes == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  auto self = *selfRes;

  // GetUint8ArrayBytes: check attachment.
  if (!self->attached(runtime)) {
    return runtime.raiseTypeError("Uint8Array is detached");
  }

  auto len = self->getLength();
  auto *data = self->data(runtime);

  // Check output length won't exceed max string length.
  uint64_t outLen = static_cast<uint64_t>(len) * 2;
  if (outLen > StringPrimitive::MAX_STRING_LENGTH) {
    return runtime.raiseRangeError("hex output exceeds maximum string length");
  }

  static const char hexChars[] = "0123456789abcdef";
  llvh::SmallVector<char, 256> out;
  out.reserve(outLen);

  for (JSTypedArrayBase::size_type i = 0; i < len; ++i) {
    out.push_back(hexChars[(data[i] >> 4) & 0x0F]);
    out.push_back(hexChars[data[i] & 0x0F]);
  }

  return StringPrimitive::createEfficient(
      runtime, ASCIIRef(out.data(), out.size()));
}

/// Uint8Array.fromBase64(string[, options])
CallResult<HermesValue> uint8ArrayFromBase64(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};

  // 1. Validate the string argument.
  auto strArg = args.getArgHandle(0);
  if (!strArg->isString()) {
    return runtime.raiseTypeError("fromBase64 argument must be a string");
  }

  bool useBase64url = false;
  LastChunkHandling lch = LastChunkHandling::Loose;
  bool omitPadding = false;
  if (parseBase64Options(
          runtime, args.getArgHandle(1), useBase64url, lch, omitPadding) ==
      ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;

  auto str = Handle<StringPrimitive>::vmcast(strArg);
  auto view = StringPrimitive::createStringView(runtime, str);

  // Decode.
  auto decoded =
      fromBase64(view, useBase64url, lch, std::numeric_limits<uint32_t>::max());

  if (decoded.error) {
    return runtime.raiseSyntaxError(TwineChar16(
        "Invalid character in base64 string at index ",
        static_cast<int32_t>(decoded.errorIndex)));
  }

  // Allocate a new Uint8Array.
  using Uint8Array = JSTypedArray<uint8_t, CellKind::Uint8ArrayKind>;
  auto arrRes = Uint8Array::allocate(runtime, decoded.bytes.size());
  if (arrRes == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  auto arr = arrRes.getValue();

  // Copy decoded bytes.
  if (!decoded.bytes.empty()) {
    std::memcpy(arr->data(runtime), decoded.bytes.data(), decoded.bytes.size());
  }

  return arr.getHermesValue();
}

/// Uint8Array.fromHex(string)
CallResult<HermesValue> uint8ArrayFromHex(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};

  auto strArg = args.getArgHandle(0);
  if (!strArg->isString()) {
    return runtime.raiseTypeError("fromHex argument must be a string");
  }

  auto str = Handle<StringPrimitive>::vmcast(strArg);
  auto view = StringPrimitive::createStringView(runtime, str);

  // The input string must have even length.
  if (view.length() % 2 != 0) {
    return runtime.raiseSyntaxError(
        "Hex string must have an even number of characters");
  }

  auto decoded = fromHexImpl(view, std::numeric_limits<uint32_t>::max());

  if (decoded.error) {
    return runtime.raiseSyntaxError("Invalid character in hex string");
  }

  using Uint8Array = JSTypedArray<uint8_t, CellKind::Uint8ArrayKind>;
  auto arrRes = Uint8Array::allocate(runtime, decoded.bytes.size());
  if (arrRes == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  auto arr = arrRes.getValue();

  if (!decoded.bytes.empty()) {
    std::memcpy(arr->data(runtime), decoded.bytes.data(), decoded.bytes.size());
  }

  return arr.getHermesValue();
}

/// Uint8Array.prototype.setFromBase64(string[, options])
CallResult<HermesValue> uint8ArrayPrototypeSetFromBase64(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};

  auto selfRes = validateUint8Array(runtime, args.getThisHandle());
  if (selfRes == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  auto self = *selfRes;

  auto strArg = args.getArgHandle(0);
  if (!strArg->isString()) {
    return runtime.raiseTypeError("setFromBase64 argument must be a string");
  }

  bool useBase64url = false;
  LastChunkHandling lch = LastChunkHandling::Loose;
  bool omitPadding = false;
  if (parseBase64Options(
          runtime, args.getArgHandle(1), useBase64url, lch, omitPadding) ==
      ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;

  // Re-check attachment since parseBase64Options runs user-observable code.
  if (!self->attached(runtime)) {
    return runtime.raiseTypeError("Uint8Array is detached");
  }

  auto str = Handle<StringPrimitive>::vmcast(strArg);
  auto view = StringPrimitive::createStringView(runtime, str);

  // Decode up to self->getLength() bytes.
  auto maxLen = self->getLength();
  auto decoded = fromBase64(view, useBase64url, lch, maxLen);

  // Per spec: write bytes into the array BEFORE checking for errors.
  uint32_t written = decoded.bytes.size();
  if (written > 0) {
    std::memcpy(self->data(runtime), decoded.bytes.data(), written);
  }

  if (decoded.error) {
    return runtime.raiseSyntaxError(TwineChar16(
        "Invalid character in base64 string at index ",
        static_cast<int32_t>(decoded.errorIndex)));
  }

  return createReadWrittenResult(runtime, decoded.read, written);
}

/// Uint8Array.prototype.setFromHex(string)
CallResult<HermesValue> uint8ArrayPrototypeSetFromHex(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};

  auto selfRes = validateUint8Array(runtime, args.getThisHandle());
  if (selfRes == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  auto self = *selfRes;

  auto strArg = args.getArgHandle(0);
  if (!strArg->isString()) {
    return runtime.raiseTypeError("setFromHex argument must be a string");
  }

  // Check attachment before accessing data.
  if (!self->attached(runtime)) {
    return runtime.raiseTypeError("Uint8Array is detached");
  }

  auto str = Handle<StringPrimitive>::vmcast(strArg);
  auto view = StringPrimitive::createStringView(runtime, str);

  // The input string must have even length.
  if (view.length() % 2 != 0) {
    return runtime.raiseSyntaxError(
        "Hex string must have an even number of characters");
  }

  auto maxLen = self->getLength();
  auto decoded = fromHexImpl(view, maxLen);

  // Per spec: write bytes into the array BEFORE checking for errors.
  uint32_t written = decoded.bytes.size();
  if (written > 0) {
    std::memcpy(self->data(runtime), decoded.bytes.data(), written);
  }

  if (decoded.error) {
    return runtime.raiseSyntaxError("Invalid character in hex string");
  }

  return createReadWrittenResult(runtime, decoded.read, written);
}

} // namespace

void populateUint8ArrayBuiltins(Runtime &runtime) {
  auto proto = Handle<JSObject>::vmcast(&runtime.Uint8ArrayPrototype);
  auto cons = Handle<JSObject>::vmcast(&runtime.Uint8ArrayConstructor);

  // Prototype methods.
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::toBase64),
      nullptr,
      uint8ArrayPrototypeToBase64,
      0);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::toHex),
      nullptr,
      uint8ArrayPrototypeToHex,
      0);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::setFromBase64),
      nullptr,
      uint8ArrayPrototypeSetFromBase64,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::setFromHex),
      nullptr,
      uint8ArrayPrototypeSetFromHex,
      1);

  // Static methods.
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::fromBase64),
      nullptr,
      uint8ArrayFromBase64,
      1);
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::fromHex),
      nullptr,
      uint8ArrayFromHex,
      1);
}

HermesValue createTypedArrayBaseConstructor(Runtime &runtime) {
  struct : public Locals {
    PinnedValue<NativeFunction> values;
    PinnedValue<NativeConstructor> cons;
    PinnedValue<> propValue;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto proto = Handle<JSObject>::vmcast(&runtime.typedArrayBasePrototype);

  // Create NativeConstructor manually to avoid global object assignment.
  // Use NativeConstructor because %TypedArray% is supposed to be
  // a constructor function object, but must not be called directly with "new".
  lv.cons = NativeConstructor::create(
      runtime,
      Handle<JSObject>::vmcast(&runtime.functionPrototype),
      nullptr,
      typedArrayBaseConstructor,
      0);

  // Define %TypedArray%.prototype to be proto.
  auto st = Callable::defineNameLengthAndPrototype(
      lv.cons,
      runtime,
      Predefined::getSymbolID(Predefined::TypedArray),
      0,
      proto,
      Callable::WritablePrototype::No);
  (void)st;
  assert(
      st != ExecutionStatus::EXCEPTION &&
      "defineNameLengthAndPrototype() failed");

  // TypedArrayBase.prototype.xxx().
  // Accessors.
  defineAccessor(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::buffer),
      nullptr,
      typedArrayPrototypeBuffer,
      nullptr,
      false,
      true);
  defineAccessor(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::byteLength),
      nullptr,
      typedArrayPrototypeByteLength,
      nullptr,
      false,
      true);
  defineAccessor(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::byteOffset),
      nullptr,
      typedArrayPrototypeByteOffset,
      nullptr,
      false,
      true);
  defineAccessor(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::length),
      nullptr,
      typedArrayPrototypeLength,
      nullptr,
      false,
      true);
  defineAccessor(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      Predefined::getSymbolID(Predefined::squareSymbolToStringTag),
      nullptr,
      typedArrayPrototypeSymbolToStringTag,
      nullptr,
      false,
      true);
  // Methods.
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::at),
      nullptr,
      typedArrayPrototypeAt,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::copyWithin),
      nullptr,
      typedArrayPrototypeCopyWithin,
      2);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::every),
      (void *)true,
      typedArrayPrototypeEverySome,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::some),
      (void *)false,
      typedArrayPrototypeEverySome,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::fill),
      nullptr,
      typedArrayPrototypeFill,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::filter),
      (void *)false,
      typedArrayPrototypeMapFilter,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::find),
      (void *)false,
      typedArrayPrototypeFind,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::findIndex),
      (void *)true,
      typedArrayPrototypeFind,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::findLast),
      (void *)false,
      typedArrayPrototypeFindLast,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::findLastIndex),
      (void *)true,
      typedArrayPrototypeFindLast,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::forEach),
      nullptr,
      typedArrayPrototypeForEach,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::includes),
      (void *)IndexOfMode::includes,
      typedArrayPrototypeIndexOf,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::indexOf),
      (void *)IndexOfMode::indexOf,
      typedArrayPrototypeIndexOf,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::lastIndexOf),
      (void *)IndexOfMode::lastIndexOf,
      typedArrayPrototypeIndexOf,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::join),
      nullptr,
      typedArrayPrototypeJoin,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::map),
      (void *)true,
      typedArrayPrototypeMapFilter,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::reduce),
      (void *)false,
      typedArrayPrototypeReduce,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::reduceRight),
      (void *)true,
      typedArrayPrototypeReduce,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::reverse),
      nullptr,
      typedArrayPrototypeReverse,
      0);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::set),
      nullptr,
      typedArrayPrototypeSet,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::slice),
      nullptr,
      typedArrayPrototypeSlice,
      2);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::sort),
      nullptr,
      typedArrayPrototypeSort,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::subarray),
      nullptr,
      typedArrayPrototypeSubarray,
      2);

  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::keys),
      (void *)IterationKind::Key,
      typedArrayPrototypeIterator,
      0);
  lv.values = defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::values),
      (void *)IterationKind::Value,
      typedArrayPrototypeIterator,
      0);

  DefinePropertyFlags dpf = DefinePropertyFlags::getNewNonEnumerableFlags();
  // Use the same valuesMethod for Symbol.iterator.
  runtime.ignoreAllocationFailure(
      JSObject::defineOwnProperty(
          proto,
          runtime,
          Predefined::getSymbolID(Predefined::SymbolIterator),
          dpf,
          lv.values));

  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::entries),
      (void *)IterationKind::Entry,
      typedArrayPrototypeIterator,
      0);

  {
    lv.propValue = runtime.ignoreAllocationFailure(
        JSObject::getNamed_RJS(
            Handle<JSArray>::vmcast(&runtime.arrayPrototype),
            runtime,
            Predefined::getSymbolID(Predefined::toString)));
    runtime.ignoreAllocationFailure(
        JSObject::defineOwnProperty(
            proto,
            runtime,
            Predefined::getSymbolID(Predefined::toString),
            dpf,
            Handle<NativeFunction>::vmcast(&lv.propValue)));
  }

  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::toLocaleString),
      nullptr,
      typedArrayPrototypeToLocaleString,
      0);

  // TypedArrayBase.xxx
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::from),
      nullptr,
      typedArrayFrom,
      1);

  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::of),
      nullptr,
      typedArrayOf,
      0);

  return lv.cons.getHermesValue();
}

#define TYPED_ARRAY(name, type)                                  \
  HermesValue create##name##ArrayConstructor(Runtime &runtime) { \
    return createTypedArrayConstructor<                          \
        type,                                                    \
        CellKind::name##ArrayKind,                               \
        name##ArrayConstructor>(runtime);                        \
  }
#include "hermes/VM/TypedArrays.def"
#undef TYPED_ARRAY

} // namespace vm
} // namespace hermes

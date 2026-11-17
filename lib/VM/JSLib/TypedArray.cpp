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
#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/JSLib/Sorting.h"
#include "hermes/VM/JSTypedArray.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/StringBuilder.h"
#include "hermes/VM/StringView.h"
#pragma GCC diagnostic push

#ifdef HERMES_COMPILER_SUPPORTS_WSHORTEN_64_TO_32
#pragma GCC diagnostic ignored "-Wshorten-64-to-32"
#endif
namespace hermes {
namespace vm {

namespace {

/// @name Abstract Operations and Helper functions
/// @{

/// ES7 22.2.2.1.1 IterableToArrayLike
CallResult<HermesValue> iterableToArrayLike(Runtime &runtime, Handle<> items) {
  // NOTE: this is a very basic function for now because iterators do not
  // yet exist in Hermes. When they do, update this function.
  return toObject(runtime, items);
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
CallResult<Handle<JSTypedArrayBase>> typedArrayCreate(
    Runtime &runtime,
    Handle<Callable> constructor,
    uint64_t length) {
  auto callRes = Callable::executeConstruct1(
      constructor,
      runtime,
      runtime.makeHandle(HermesValue::encodeUntrustedNumberValue(length)));
  if (callRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  PseudoHandle<> retval = std::move(*callRes);
  if (!vmisa<JSTypedArrayBase>(retval.get())) {
    return runtime.raiseTypeError(
        "The constructor needs to construct a TypedArray");
  }
  auto newTypedArray =
      Handle<JSTypedArrayBase>::vmcast(runtime.makeHandle(std::move(retval)));
  // If `argumentList` is a single number, then
  // If the value of newTypedArray's [[ArrayLength]] internal slot <
  // argumentList[0], throw a TypeError exception.
  if (LLVM_UNLIKELY(newTypedArray->getLength() < length)) {
    return runtime.raiseTypeError(
        "TypedArray constructor created an array that was too small");
  }
  return newTypedArray;
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

// ES6 22.2.1.3
template <typename T, CellKind C>
CallResult<HermesValue> typedArrayConstructorFromTypedArray(
    Runtime &runtime,
    Handle<JSTypedArray<T, C>> self,
    Handle<JSTypedArrayBase> other) {
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

// ES6 22.2.1.5
template <typename T, CellKind C>
CallResult<HermesValue> typedArrayConstructorFromArrayBuffer(
    Runtime &runtime,
    Handle<JSTypedArray<T, C>> self,
    Handle<JSArrayBuffer> buffer,
    Handle<> byteOffset,
    Handle<> length) {
  // This differs from step 7 of the spec, which requires `ToInteger` instead
  // of `ToIndex`; however, we have to bound offset to be fittable into a
  // 64-bit integer to avoid overflow or loss of precision
  auto res = toIndex(runtime, byteOffset);
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t offset = res->getNumberAs<uint64_t>();
  if (offset % sizeof(T) != 0) {
    return runtime.raiseRangeError(
        "new TypedArray(buffer, [byteOffset], "
        "[length]): if byteOffset is specified, it "
        "must be evenly divisible by the element size");
  }
  auto bufferByteLength = buffer->size();
  uint64_t newByteLength = 0;
  if (length->isUndefined()) {
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
    auto res2 = toLength(runtime, length);
    if (res2 == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
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
  // Steps 1 & 2 already covered by caller.
  // 5. Let arrayLike be ? IterableToArrayLike(object).
  auto objRes = iterableToArrayLike(runtime, obj);
  if (objRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto arrayLike = runtime.makeHandle<JSObject>(objRes.getValue());
  // 6. Let len be ? ToLength(? Get(arrayLike, "length")).
  auto propRes = JSObject::getNamed_RJS(
      arrayLike, runtime, Predefined::getSymbolID(Predefined::length));
  if (propRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto intRes = toLength(runtime, runtime.makeHandle(std::move(*propRes)));
  if (intRes == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;
  uint64_t len = intRes->getNumberAs<uint64_t>();
  // 4. Let O be ? AllocateTypedArray(constructorName, NewTarget,
  // "%TypedArrayPrototype%").
  // 7. Perform ? AllocateTypedArrayBuffer(O, len).
  if (JSTypedArray<T, C>::createBuffer(runtime, self, len) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  GCScope scope(runtime);
  // 8. Let k be 0.
  MutableHandle<HermesValue> i(
      runtime, HermesValue::encodeUntrustedNumberValue(0));
  auto marker = scope.createMarker();
  // 9. Repeat, while k < len.
  for (; i->getNumberAs<uint64_t>() < len;
       i = HermesValue::encodeUntrustedNumberValue(
           i->getNumberAs<uint64_t>() + 1)) {
    // a. Let Pk be ! ToString(k).
    // b. Let kValue be ? Get(arrayLike, Pk).
    // c. Perform ? Set(O, Pk, kValue, true).
    if ((propRes = JSObject::getComputed_RJS(arrayLike, runtime, i)) ==
            ExecutionStatus::EXCEPTION ||
        JSTypedArray<T, C>::putComputed_RJS(
            self, runtime, i, runtime.makeHandle(std::move(*propRes))) ==
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
CallResult<HermesValue>
typedArrayConstructor(void *, Runtime &runtime, NativeArgs args) {
  // 1. If NewTarget is undefined, throw a TypeError exception.
  if (!args.isConstructorCall()) {
    return runtime.raiseTypeError(
        "JSTypedArray() called in function context instead of constructor");
  }
  auto self = args.vmcastThis<JSTypedArray<T, C>>();
  if (args.getArgCount() == 0) {
    // ES6 22.2.1.1
    if (JSTypedArray<T, C>::createBuffer(runtime, self, 0) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    return self.getHermesValue();
  }
  auto firstArg = args.getArgHandle(0);
  if (!firstArg->isObject()) {
    return typedArrayConstructorFromLength<T, C>(runtime, self, firstArg);
  }
  if (auto otherTA = Handle<JSTypedArrayBase>::dyn_vmcast(firstArg)) {
    return typedArrayConstructorFromTypedArray<T, C>(runtime, self, otherTA);
  }
  if (auto buffer = Handle<JSArrayBuffer>::dyn_vmcast(firstArg)) {
    return typedArrayConstructorFromArrayBuffer<T, C>(
        runtime, self, buffer, args.getArgHandle(1), args.getArgHandle(2));
  }
  return typedArrayConstructorFromObject<T, C>(runtime, self, firstArg);
}

template <typename T, CellKind C, NativeFunctionPtr Ctor>
Handle<JSObject> createTypedArrayConstructor(Runtime &runtime) {
  using TA = JSTypedArray<T, C>;
  auto proto = TA::getPrototype(runtime);

  auto cons = defineSystemConstructor(
      runtime,
      TA::getName(runtime),
      Ctor,
      proto,
      Handle<JSObject>::vmcast(&runtime.typedArrayBaseConstructor),
      3,
      NativeConstructor::creatorFunction<TA>,
      C);

  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.enumerable = 0;
  dpf.configurable = 0;
  dpf.writable = 0;

  auto bytesPerElement =
      runtime.makeHandle(HermesValue::encodeUntrustedNumberValue(sizeof(T)));
  // %TypedArray%.prototype.xxx.
  defineProperty(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::BYTES_PER_ELEMENT),
      bytesPerElement,
      dpf);

  // %TypedArray%.xxx.
  defineProperty(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::BYTES_PER_ELEMENT),
      bytesPerElement,
      dpf);
  return cons;
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
  MutableHandle<> storage(runtime);
  MutableHandle<> val{runtime};
  GCScopeMarkerRAII marker{runtime};
  for (JSTypedArrayBase::size_type i = 0; i < len; ++i) {
    if (!self->attached(runtime)) {
      // If the callback detached this TypedArray, raise a TypeError and don't
      // continue.
      return runtime.raiseTypeError("Detached the TypedArray in the callback");
    }
    val = JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, i);
    auto callRes = Callable::executeCall3(
        callbackfn,
        runtime,
        thisArg,
        *val,
        HermesValue::encodeUntrustedNumberValue(i),
        self.getHermesValue());
    if (callRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    if (MapOrFilter) {
      // Map adds the result of the callback onto the array.
      storage = std::move(*callRes);
      JSArray::setElementAt(values, runtime, insert++, storage);
    } else if (toBoolean(callRes->get())) {
      storage = *val;
      JSArray::setElementAt(values, runtime, insert++, storage);
    }
    marker.flush();
  }
  return HermesValue::encodeUntrustedNumberValue(insert);
}

/// This is the sort model for use with TypedArray.prototype.sort.
/// template param \p WithCompareFn should be true if the compare function is
/// a valid callback to call, and false if it is null or undefined.
template <bool WithCompareFn>
class TypedArraySortModel : public SortModel {
 protected:
  /// Runtime to sort in.
  Runtime &runtime_;

  /// Scope to allocate handles in, gets destroyed with this.
  GCScope gcScope_;

  /// JS comparison function, return -1 for less, 0 for equal, 1 for greater.
  /// If null, then use the built in < operator.
  Handle<Callable> compareFn_;

  /// Object to sort.
  Handle<JSTypedArrayBase> self_;

  MutableHandle<HermesValue> aHandle_;
  MutableHandle<HermesValue> bHandle_;

  /// Marker created after initializing all fields so handles allocated later
  /// can be flushed.
  GCScope::Marker gcMarker_;

 public:
  TypedArraySortModel(
      Runtime &runtime,
      Handle<JSTypedArrayBase> obj,
      Handle<Callable> compareFn)
      : runtime_(runtime),
        gcScope_(runtime),
        compareFn_(compareFn),
        self_(obj),
        aHandle_(runtime),
        bHandle_(runtime),
        gcMarker_(gcScope_.createMarker()) {}

  // Swap elements at indices a and b.
  virtual ExecutionStatus swap(uint32_t a, uint32_t b) override {
    aHandle_ =
        JSObject::getOwnIndexed(createPseudoHandle(self_.get()), runtime_, a);
    bHandle_ =
        JSObject::getOwnIndexed(createPseudoHandle(self_.get()), runtime_, b);
    if (JSObject::setOwnIndexed(self_, runtime_, a, bHandle_) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    if (JSObject::setOwnIndexed(self_, runtime_, b, aHandle_) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    return ExecutionStatus::RETURNED;
  }

  // Compare elements at index a and at index b.
  virtual CallResult<int> compare(uint32_t a, uint32_t b) override {
    GCScopeMarkerRAII gcMarker{gcScope_, gcMarker_};

    CallResult<PseudoHandle<HermesValue>> callRes{ExecutionStatus::EXCEPTION};
    {
      Handle<> aValHandle = runtime_.makeHandle(
          JSObject::getOwnIndexed(
              createPseudoHandle(self_.get()), runtime_, a));
      // To avoid the need to create a handle for bVal a NoAllocScope is created
      // below, to ensure no memory allocation will happen.
      HermesValue bVal =
          JSObject::getOwnIndexed(createPseudoHandle(self_.get()), runtime_, b);

      // N.B.: aVal needs to be initialized after bVal's initialization -- i.e.,
      // after no more allocations are expected for a while.
      HermesValue aVal = *aValHandle;

      {
        NoAllocScope noAllocs{runtime_};
        if (!WithCompareFn) {
          if (LLVM_UNLIKELY(aVal.isBigInt())) {
            return aVal.getBigInt()->compare(bVal.getBigInt());
          } else {
            double aNum = aVal.getNumber();
            double bNum = bVal.getNumber();
            if (LLVM_UNLIKELY(aNum == 0) && LLVM_UNLIKELY(bNum == 0) &&
                LLVM_UNLIKELY(std::signbit(aNum)) &&
                LLVM_UNLIKELY(!std::signbit(bNum))) {
              // -0 < +0, according to the spec.
              return -1;
            }
            return (aNum < bNum) ? -1 : (aNum > bNum ? 1 : 0);
          }
          assert(
              compareFn_ && "Cannot use this version if the compareFn is null");
        }
      }
      // ES7 22.2.3.26 2a.
      // Let v be toNumber_RJS(Call(comparefn, undefined, x, y)).
      callRes = Callable::executeCall2(
          compareFn_, runtime_, Runtime::getUndefinedValue(), aVal, bVal);
    }

    if (callRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    auto intRes =
        toNumber_RJS(runtime_, runtime_.makeHandle(std::move(*callRes)));
    if (intRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    // ES7 22.2.3.26 2b.
    // If IsDetachedBuffer(buffer) is true, throw a TypeError exception.
    if (LLVM_UNLIKELY(!self_->attached(runtime_))) {
      return runtime_.raiseTypeError("Callback to sort() detached the array");
    }
    // Cannot return intRes's value directly because it can be NaN
    auto res = intRes->getNumber();
    return (res < 0) ? -1 : (res > 0 ? 1 : 0);
  }
};

// ES7 22.2.3.23.1
CallResult<HermesValue> typedArrayPrototypeSetObject(
    Runtime &runtime,
    Handle<JSTypedArrayBase> self,
    Handle<> obj,
    double offset) {
  double targetLength = self->getLength();
  auto objRes = toObject(runtime, obj);
  if (objRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto src = runtime.makeHandle<JSObject>(objRes.getValue());
  auto propRes = JSObject::getNamed_RJS(
      src, runtime, Predefined::getSymbolID(Predefined::length));
  if (propRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto intRes = toLength(runtime, runtime.makeHandle(std::move(*propRes)));
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
  MutableHandle<> k(runtime, HermesValue::encodeUntrustedNumberValue(0));
  auto marker = scope.createMarker();
  for (; k->getNumberAs<uint64_t>() < srcLength;
       k = HermesValue::encodeUntrustedNumberValue(
           k->getNumberAs<uint64_t>() + 1)) {
    if ((propRes = JSObject::getComputed_RJS(src, runtime, k)) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    auto kValue = runtime.makeHandle(std::move(*propRes));
    if (JSObject::setOwnIndexed(self, runtime, offset++, kValue) ==
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
  if (self->getBuffer(runtime)->getDataBlock(runtime) !=
      src->getBuffer(runtime)->getDataBlock(runtime)) {
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
  if (JSTypedArrayBase::setToCopyOfBuffer(
          runtime,
          newSrc,
          0,
          runtime.makeHandle(src->getBuffer(runtime)),
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

CallResult<HermesValue>
typedArrayBaseConstructor(void *, Runtime &runtime, NativeArgs) {
  return runtime.raiseTypeError(
      "TypedArray is abstract, it cannot be constructed");
}

/// @}

#define TYPED_ARRAY(name, type)                                    \
  CallResult<HermesValue> name##ArrayConstructor(                  \
      void *ctx, Runtime &rt, NativeArgs args) {                   \
    return typedArrayConstructor<type, CellKind::name##ArrayKind>( \
        ctx, rt, args);                                            \
  }
#include "hermes/VM/TypedArrays.def"
#undef TYPED_ARRAY

/// ES7 22.2.2.1
CallResult<HermesValue>
typedArrayFrom(void *, Runtime &runtime, NativeArgs args) {
  auto source = args.getArgHandle(0);
  CallResult<bool> isConstructorRes = isConstructor(runtime, args.getThisArg());
  if (LLVM_UNLIKELY(isConstructorRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 1. Let C be the this value.
  if (!*isConstructorRes) {
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
  auto arrayLike = runtime.makeHandle<JSObject>(objRes.getValue());
  // 7. Let len be ? ToLength(? Get(arrayLike, "length")).
  auto propRes = JSObject::getNamed_RJS(
      arrayLike, runtime, Predefined::getSymbolID(Predefined::length));
  if (propRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto intRes = toLength(runtime, runtime.makeHandle(std::move(*propRes)));
  if (intRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t len = intRes.getValue().getNumberAs<uint64_t>();
  // 8. Let targetObj be ? TypedArrayCreate(C, len).
  auto targetObj = typedArrayCreate(runtime, C, len);
  if (targetObj == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 9. Let k be 0.
  MutableHandle<> k(runtime, HermesValue::encodeUntrustedNumberValue(0));
  // 10. Repeat, while k < len.
  for (; k->getNumberAs<uint64_t>() < len;
       k = HermesValue::encodeUntrustedNumberValue(
           k->getNumberAs<uint64_t>() + 1)) {
    GCScopeMarkerRAII marker{runtime};
    // a - b. Get the value of the property at k.
    if ((propRes = JSObject::getComputed_RJS(arrayLike, runtime, k)) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    // c. If mapping is true, then
    if (mapfn) {
      // i. Let mappedValue be ? Call(mapfn, T, [kValue, k]).
      auto callRes = Callable::executeCall2(
          mapfn, runtime, T, propRes->get(), k.getHermesValue());
      if (callRes == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      propRes = std::move(callRes);
    }
    // NOTE: The returned value is either the same as the getComputed call, or
    // the call to the mapfn, so either way it is the correct value.
    // d. Else, let mappedValue be kValue (already done by initializer).
    auto mappedValue = runtime.makeHandle(std::move(*propRes));
    // e. Perform ? Set(targetObj, Pk, mappedValue, true).
    if (JSObject::putComputed_RJS(*targetObj, runtime, k, mappedValue) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    // f. Increase k by 1. (In the loop increment).
  }
  // 11. Return targetObj.
  return targetObj->getHermesValue();
}

/// ES7 22.2.2.2
CallResult<HermesValue>
typedArrayOf(void *, Runtime &runtime, NativeArgs args) {
  // 1. Let len be the actual number of arguments passed to this function.
  uint64_t len = args.getArgCount();
  // 2. Let items be the List of arguments passed to this function. (args is
  // items).
  // 3. Let C be the this value.
  CallResult<bool> isConstructorRes = isConstructor(runtime, args.getThisArg());
  if (LLVM_UNLIKELY(isConstructorRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (!*isConstructorRes) {
    // 4. If IsConstructor(C) is false, throw a TypeError exception.
    return runtime.raiseTypeError(
        "Cannot invoke %TypedArray%.of when %TypedArray% is not a constructor "
        "function");
  }
  auto C = Handle<Callable>::vmcast(args.getThisHandle());
  // 5. Let newObj be ? TypedArrayCreate(C, len).
  auto newObj = typedArrayCreate(runtime, C, len);
  if (newObj == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 6. Let k be 0.
  MutableHandle<> k(runtime, HermesValue::encodeUntrustedNumberValue(0));
  GCScope scope(runtime);
  auto marker = scope.createMarker();
  // 7. Repeat, while k < len.
  for (; k->getNumberAs<uint64_t>() < len;
       k = HermesValue::encodeUntrustedNumberValue(
           k->getNumberAs<uint64_t>() + 1)) {
    // a. Let kValue be items[k].
    auto kValue = args.getArg(k->getNumberAs<uint64_t>());
    // b. Let Pk be ! ToString(k).
    // c. Perform ? Set(newObj, Pk, kValue, true).
    if (JSObject::putComputed_RJS(
            *newObj, runtime, k, runtime.makeHandle(kValue)) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    // d. Increase k by 1. (In the loop increment).
    scope.flushToMarker(marker);
  }
  // Return newObj.
  return newObj->getHermesValue();
}
/// @}

/// @name %JSTypedArray%.prototype
/// @{

/// ES6 22.2.3.1
CallResult<HermesValue>
typedArrayPrototypeBuffer(void *, Runtime &runtime, NativeArgs args) {
  if (JSTypedArrayBase::validateTypedArray(
          runtime, args.getThisHandle(), false) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  return HermesValue::encodeObjectValue(self->getBuffer(runtime));
}

CallResult<HermesValue>
typedArrayPrototypeByteLength(void *, Runtime &runtime, NativeArgs args) {
  if (JSTypedArrayBase::validateTypedArray(
          runtime, args.getThisHandle(), false) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  return HermesValue::encodeUntrustedNumberValue(
      self->attached(runtime) ? self->getByteLength() : 0);
}

/// ES6 22.2.3.3
CallResult<HermesValue>
typedArrayPrototypeByteOffset(void *, Runtime &runtime, NativeArgs args) {
  if (JSTypedArrayBase::validateTypedArray(
          runtime, args.getThisHandle(), false) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  return HermesValue::encodeUntrustedNumberValue(
      self->attached(runtime) && self->getLength() != 0 ? self->getByteOffset()
                                                        : 0);
}

/// ES6 23.2.3.1
CallResult<HermesValue>
typedArrayPrototypeAt(void *, Runtime &runtime, NativeArgs args) {
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

  // 8. Return ? Get(O, ! ToString(𝔽(k))).
  // Since we know we have a TypedArray, we can directly call JSTypedArray::at
  // rather than getComputed_RJS like the spec mandates.
#define TYPED_ARRAY(name, type)                                            \
  case CellKind::name##ArrayKind: {                                        \
    auto *arr = vmcast<JSTypedArray<type, CellKind::name##ArrayKind>>(*O); \
    if (!arr->attached(runtime)) {                                         \
      return runtime.raiseTypeError("Underlying ArrayBuffer detached");    \
    }                                                                      \
    return HermesValue::encodeUntrustedNumberValue(arr->at(runtime, k));   \
  }
  switch (O->getKind()) {
#include "hermes/VM/TypedArrays.def"
    default:
      llvm_unreachable("Invalid TypedArray after ValidateTypedArray call");
  }
}

/// ES6 22.2.3.5
CallResult<HermesValue>
typedArrayPrototypeCopyWithin(void *, Runtime &runtime, NativeArgs args) {
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

  // Need to case on the TypedArray type to avoid encoding using HermesValues.
  // We need to preserve the bit-level encoding of values, and HermesValues
  // destroy information, e.g. which NaN is being used.
#define TYPED_ARRAY(name, type)                                            \
  case CellKind::name##ArrayKind: {                                        \
    auto *arr = vmcast<JSTypedArray<type, CellKind::name##ArrayKind>>(*O); \
    if (!arr->attached(runtime)) {                                         \
      return runtime.raiseTypeError(                                       \
          "Underlying ArrayBuffer detached after calling copyWithin");     \
    }                                                                      \
    while (count > 0) {                                                    \
      arr->at(runtime, to) = arr->at(runtime, from);                       \
      from += direction;                                                   \
      to += direction;                                                     \
      --count;                                                             \
    }                                                                      \
    break;                                                                 \
  }

  switch (O->getKind()) {
#include "hermes/VM/TypedArrays.def"
    default:
      llvm_unreachable("Invalid TypedArray after ValidateTypedArray call");
  }

  return O.getHermesValue();
}

// ES6 22.2.3.7 and 22.2.3.25 (also see Array.prototype.every/some)
CallResult<HermesValue>
typedArrayPrototypeEverySome(void *ctx, Runtime &runtime, NativeArgs args) {
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
        HermesValue::encodeUntrustedNumberValue(i),
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
CallResult<HermesValue>
typedArrayPrototypeFill(void *, Runtime &runtime, NativeArgs args) {
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
  auto value = runtime.makeHandle(res.getValue());
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

  if (JSObject::setOwnIndexed(self, runtime, k, value) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto elementSize = self->getByteWidth();
  uint8_t *begin = self->begin(runtime);
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
  MutableHandle<> val{runtime};
  GCScope gcScope(runtime);
  auto marker = gcScope.createMarker();
  for (JSTypedArrayBase::size_type counter = 0; counter < len; counter++) {
    auto i = reverse ? (len - counter - 1) : counter;
    val = JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, i);
    auto idx = HermesValue::encodeUntrustedNumberValue(i);
    auto callRes = Callable::executeCall3(
        callbackfn, runtime, thisArg, *val, idx, self.getHermesValue());
    if (callRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    if (toBoolean(callRes->get())) {
      // Found one, return it.
      return index ? idx : *val;
    }
    gcScope.flushToMarker(marker);
  }
  return index ? HermesValue::encodeUntrustedNumberValue(-1)
               : HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue>
typedArrayPrototypeFind(void *ctx, Runtime &runtime, NativeArgs args) {
  return typedFindHelper(ctx, false, runtime, args);
}

CallResult<HermesValue>
typedArrayPrototypeFindLast(void *ctx, Runtime &runtime, NativeArgs args) {
  return typedFindHelper(ctx, true, runtime, args);
}

CallResult<HermesValue>
typedArrayPrototypeForEach(void *, Runtime &runtime, NativeArgs args) {
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
    // The callback function can detach the TypedArray.
    if (!self->attached(runtime)) {
      return runtime.raiseTypeError("Detached the ArrayBuffer in the callback");
    }
    HermesValue val =
        JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, i);
    if (Callable::executeCall3(
            callbackfn,
            runtime,
            thisArg,
            val,
            HermesValue::encodeUntrustedNumberValue(i),
            self.getHermesValue()) == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    gcScope.flushToMarker(marker);
  }
  return HermesValue::encodeUndefinedValue();
}

enum class IndexOfMode { includes, indexOf, lastIndexOf };
CallResult<HermesValue>
typedArrayPrototypeIndexOf(void *ctx, Runtime &runtime, NativeArgs args) {
  const auto indexOfMode = *reinterpret_cast<const IndexOfMode *>(&ctx);
  // indexOfMode stores Whether this call is "includes", "indexOf", or
  // "lastIndexOf".
  auto ret = [indexOfMode](bool x = false, double y = -1) {
    switch (indexOfMode) {
      case IndexOfMode::includes:
        return HermesValue::encodeBoolValue(x);
      default:
        return HermesValue::encodeUntrustedNumberValue(y);
    }
  };
  if (JSTypedArrayBase::validateTypedArray(runtime, args.getThisHandle()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  double len = self->getLength();
  if (len == 0) {
    return ret();
  }
  auto searchElement = args.getArgHandle(0);
  if (!searchElement->isNumber() && !searchElement->isBigInt()) {
    // If it's not a number, nothing will match.
    return ret();
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
    if (LLVM_UNLIKELY(!self->attached(runtime))) {
      // If the ToInteger call detached this TypedArray, raise a TypeError and
      // don't continue.
      return runtime.raiseTypeError("Detached the TypedArray in the callback");
    }
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
  for (; inRange(k, len); k += delta) {
    HermesValue curr =
        JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, k);
    NoAllocScope noAllocs{runtime};

    bool comp = indexOfMode == IndexOfMode::includes
        ? isSameValueZero(curr, *searchElement)
        : strictEqualityTest(curr, *searchElement);
    if (comp) {
      return ret(true, k);
    }
  }
  return ret();
}

CallResult<HermesValue>
typedArrayPrototypeIterator(void *ctx, Runtime &runtime, NativeArgs args) {
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

CallResult<HermesValue>
typedArrayPrototypeMapFilter(void *ctx, Runtime &runtime, NativeArgs args) {
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
  // Can't use a vector since this could store an unbounded number of handles.
  auto arrRes = JSArray::create(runtime, len, 0);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto values = *arrRes;
  JSTypedArrayBase::size_type insert = 0;
  CallResult<HermesValue> res{ExecutionStatus::EXCEPTION};
  if (map) {
    if ((res = mapFilterLoop<true>(
             runtime, self, callbackfn, thisArg, values, insert, len)) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
  } else {
    if ((res = mapFilterLoop<false>(
             runtime, self, callbackfn, thisArg, values, insert, len)) ==
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
  MutableHandle<> storage(runtime);
  auto marker = gcScope.createMarker();
  for (JSTypedArrayBase::size_type i = 0; i < insert; ++i) {
    storage = values->at(runtime, i).unboxToHV(runtime);
    if (JSObject::setOwnIndexed(TA, runtime, i, storage) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    gcScope.flushToMarker(marker);
  }
  return TA.getHermesValue();
}

/// ES6 22.2.3.17
CallResult<HermesValue>
typedArrayPrototypeLength(void *, Runtime &runtime, NativeArgs args) {
  if (JSTypedArrayBase::validateTypedArray(
          runtime, args.getThisHandle(), false) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  return HermesValue::encodeUntrustedNumberValue(
      self->attached(runtime) ? self->getLength() : 0);
}

CallResult<HermesValue>
typedArrayPrototypeJoin(void *, Runtime &runtime, NativeArgs args) {
  // NOTE: there are probably some optimizations that can be made here due to
  // operating on only numbers in typed arrays.
  if (JSTypedArrayBase::validateTypedArray(runtime, args.getThisHandle()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  auto len = self->getLength();
  auto separator = args.getArg(0).isUndefined()
      ? runtime.makeHandle(
            HermesValue::encodeStringValue(
                runtime.getPredefinedString(Predefined::comma)))
      : args.getArgHandle(0);
  auto res = toString_RJS(runtime, separator);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto sep = runtime.makeHandle(std::move(*res));
  if (len == 0) {
    // Quick exit for empty arrays to avoid allocations.
    // NOTE: this needs to come after the `toString` call on the separator
    // according to the spec.
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::emptyString));
  }

  if (len > std::numeric_limits<uint32_t>::max() ||
      sep->getStringLength() >
          (double)StringPrimitive::MAX_STRING_LENGTH / len) {
    // Check for overflow.
    return runtime.raiseRangeError(
        "String.prototype.repeat result exceeds limit");
  }

  // Final size of the resultant string.
  // Its safe to multiply as overflow check is done above
  SafeUInt32 size(sep->getStringLength() * (len - 1));

  // Storage for the strings for each element.
  auto arrRes = JSArray::create(runtime, len, 0);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto strings = *arrRes;

  // Call toString on all the elements of the array.
  {
    // Make sure to drop the Handle early, it's not needed outside of the loop.
    MutableHandle<> elem(runtime);
    for (decltype(len) i = 0; i < len; ++i) {
      GCScope gcScope(runtime);
      elem =
          JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, i);

      auto res2 = toString_RJS(runtime, elem);
      if (LLVM_UNLIKELY(res2 == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      auto S = runtime.makeHandle(std::move(*res2));
      size.add(S->getStringLength());
      JSArray::setElementAt(strings, runtime, i, S);
    }
  }

  // Allocate the complete result.
  auto builder = StringBuilder::createStringBuilder(runtime, size);
  if (builder == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  MutableHandle<StringPrimitive> element{runtime};
  element = strings->at(runtime, 0).getString(runtime);
  builder->appendStringPrim(element);
  // Copy the strings.
  for (decltype(len) i = 1; i < len; ++i) {
    builder->appendStringPrim(sep);
    element = strings->at(runtime, i).getString(runtime);
    builder->appendStringPrim(element);
  }
  return HermesValue::encodeStringValue(*builder->getStringPrimitive());
}

CallResult<HermesValue>
typedArrayPrototypeReduce(void *ctx, Runtime &runtime, NativeArgs args) {
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
  // If the intial value is not provided, it must be the first value from the
  // array.
  MutableHandle<> accumulator(runtime);
  if (calledWithInitialValue) {
    accumulator = args.getArg(1);
  } else {
    accumulator = JSObject::getOwnIndexed(
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
    if (!self->attached(runtime)) {
      // If the callback detached this TypedArray, raise a TypeError and don't
      // continue.
      return runtime.raiseTypeError("Detached the TypedArray in the callback");
    }
    HermesValue val =
        JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, i);
    auto callRes = Callable::executeCall4(
        callbackfn,
        runtime,
        undefinedThis,
        accumulator.getHermesValue(),
        val,
        HermesValue::encodeUntrustedNumberValue(i),
        self.getHermesValue());
    if (callRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    accumulator = std::move(*callRes);
    scope.flushToMarker(marker);
  }
  return accumulator.getHermesValue();
}

// ES7 22.2.3.22
CallResult<HermesValue>
typedArrayPrototypeReverse(void *, Runtime &runtime, NativeArgs args) {
  if (JSTypedArrayBase::validateTypedArray(runtime, args.getThisHandle()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  const JSTypedArrayBase::size_type len = self->getLength();
  const JSTypedArrayBase::size_type middle = len / 2;
  MutableHandle<> lowerHandle(runtime);
  MutableHandle<> upperHandle(runtime);
  for (JSTypedArrayBase::size_type lower = 0; lower != middle; ++lower) {
    auto upper = len - lower - 1;
    lowerHandle =
        JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, lower);
    upperHandle =
        JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, upper);
    if (JSObject::setOwnIndexed(self, runtime, lower, upperHandle) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    if (JSObject::setOwnIndexed(self, runtime, upper, lowerHandle) ==
        ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return self.getHermesValue();
}

/// ES7 22.2.3.26
CallResult<HermesValue>
typedArrayPrototypeSort(void *, Runtime &runtime, NativeArgs args) {
  if (JSTypedArrayBase::validateTypedArray(runtime, args.getThisHandle()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  const JSTypedArrayBase::size_type len = self->getLength();

  // Null if not a callable compareFn.
  auto compareFn = Handle<Callable>::dyn_vmcast(args.getArgHandle(0));
  if (!args.getArg(0).isUndefined() && !compareFn) {
    return runtime.raiseTypeError("TypedArray sort argument must be callable");
  }

  // Use our custom sort routine. We can't use std::sort because it performs
  // optimizations that allow it to bypass calls to std::swap, but our swap
  // function is special, since it needs to use the internal Object functions.
  if (compareFn) {
    TypedArraySortModel<true> sm(runtime, self, compareFn);
    if (LLVM_UNLIKELY(quickSort(&sm, 0, len) == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  } else {
    TypedArraySortModel<false> sm(runtime, self, compareFn);
    if (LLVM_UNLIKELY(quickSort(&sm, 0, len) == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }
  return self.getHermesValue();
}

// ES7 22.2.3.23
CallResult<HermesValue>
typedArrayPrototypeSet(void *, Runtime &runtime, NativeArgs args) {
  if (JSTypedArrayBase::validateTypedArray(
          runtime, args.getThisHandle(), false) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto self = args.vmcastThis<JSTypedArrayBase>();
  // Default to zero if unspecified.
  auto offset = runtime.makeHandle(
      args.getArgCount() >= 2 ? args.getArg(1)
                              : HermesValue::encodeUntrustedNumberValue(0));
  auto res = toIntegerOrInfinity(runtime, offset);
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
CallResult<HermesValue>
typedArrayPrototypeSlice(void *, Runtime &runtime, NativeArgs args) {
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
CallResult<HermesValue>
typedArrayPrototypeSubarray(void *, Runtime &runtime, NativeArgs args) {
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
    Runtime &runtime,
    NativeArgs args) {
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

CallResult<HermesValue>
typedArrayPrototypeToLocaleString(void *, Runtime &runtime, NativeArgs args) {
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

  // Array to store each of the strings of the elements.
  auto arrRes = JSArray::create(runtime, len, len);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto strings = *arrRes;

  // Index into the array.
  MutableHandle<> storage(runtime);

  auto marker = gcScope.createMarker();
  for (JSTypedArrayBase::size_type i = 0; i < len; ++i) {
    storage =
        JSObject::getOwnIndexed(createPseudoHandle(self.get()), runtime, i);
    auto objRes = toObject(runtime, storage);
    if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto elementObj = runtime.makeHandle<JSObject>(objRes.getValue());

    // Retrieve the toLocaleString function.
    auto propRes = JSObject::getNamed_RJS(
        elementObj,
        runtime,
        Predefined::getSymbolID(Predefined::toLocaleString));
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (auto func = Handle<Callable>::dyn_vmcast(
            runtime.makeHandle(std::move(*propRes)))) {
#ifdef HERMES_ENABLE_INTL
      auto callRes = Callable::executeCall2(
          func, runtime, elementObj, args.getArg(0), args.getArg(1));
#else
      auto callRes = Callable::executeCall0(func, runtime, elementObj);
#endif
      if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      auto strRes =
          toString_RJS(runtime, runtime.makeHandle(std::move(*callRes)));
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      auto elementStr = runtime.makeHandle(std::move(*strRes));
      JSArray::setElementAt(strings, runtime, i, elementStr);
      size.add(elementStr->getStringLength());
    } else {
      return runtime.raiseTypeError("toLocaleString() not callable");
    }
    gcScope.flushToMarker(marker);
  }

  auto builder = StringBuilder::createStringBuilder(runtime, size);
  if (builder == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  MutableHandle<StringPrimitive> element{runtime};
  element = strings->at(runtime, 0).getString(runtime);
  builder->appendStringPrim(element);

  for (uint32_t i = 1; i < len; ++i) {
    // Every element after the first needs a separator before it.
    builder->appendASCIIRef(separator);
    element = strings->at(runtime, i).getString(runtime);
    builder->appendStringPrim(element);
  }
  return HermesValue::encodeStringValue(*builder->getStringPrimitive());
}

/// ES14.0 23.2.3.32
CallResult<HermesValue>
typedArrayPrototypeToReversed(void *, Runtime &runtime, NativeArgs args) {
  GCScope gcScope{runtime};

  // 2. Perform ? ValidateTypedArray(O).
  if (JSTypedArrayBase::validateTypedArray(runtime, args.getThisHandle()) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  // 1. Let O be this value
  auto self = args.vmcastThis<JSTypedArrayBase>();

  // 3. Let len be O.[[ArrayLength]].
  auto len = self->getLength();
  auto byteLength = self->getByteLength();
  auto byteWidth = self->getByteWidth();

  // 4. Let A be ? TypedArrayCreateSameType(O, « 𝔽(len) »).
  auto aRes = JSTypedArrayBase::allocateSpecies(runtime, self, len);
  if (LLVM_UNLIKELY(aRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto A = aRes.getValue();

  // 5. Let k be 0.
  JSArrayBuffer::size_type k = 0;

  auto dstBlock = A->getBuffer(runtime)->getDataBlock(runtime);
  auto srcBlock = self->getBuffer(runtime)->getDataBlock(runtime);

  // 6. Repeat, while k < len,
  while (k < byteLength) {
    // 6a. Let from be ! ToString(𝔽(length - k - 1)).
    size_t from = byteLength - k - byteWidth;

    // 6d. Perform ! Set(A, Pk, fromValue, true).
    memcpy(dstBlock + k, srcBlock + from, byteWidth);

    // 6e. Set k to k + 1.
    k += byteWidth;
  }

  return A.getHermesValue();
}

Handle<JSObject> createTypedArrayBaseConstructor(Runtime &runtime) {
  auto proto = Handle<JSObject>::vmcast(&runtime.typedArrayBasePrototype);

  // Create NativeConstructor manually to avoid global object assignment.
  // Use NativeConstructor because %TypedArray% is supposed to be
  // a constructor function object, but must not be called directly with "new".
  auto cons = runtime.makeHandle(
      NativeConstructor::create(
          runtime,
          Handle<JSObject>::vmcast(&runtime.functionPrototype),
          nullptr,
          typedArrayBaseConstructor,
          0,
          NativeConstructor::creatorFunction<JSObject>,
          CellKind::JSObjectKind));

  // Define %TypedArray%.prototype to be proto.
  auto st = Callable::defineNameLengthAndPrototype(
      cons,
      runtime,
      Predefined::getSymbolID(Predefined::TypedArray),
      0,
      proto,
      Callable::WritablePrototype::No,
      false);
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
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::values),
      (void *)IterationKind::Value,
      typedArrayPrototypeIterator,
      0);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::entries),
      (void *)IterationKind::Entry,
      typedArrayPrototypeIterator,
      0);

  DefinePropertyFlags dpf = DefinePropertyFlags::getNewNonEnumerableFlags();

  // Use the same valuesMethod for Symbol.iterator.
  {
    auto propValue = runtime.ignoreAllocationFailure(
        JSObject::getNamed_RJS(
            proto, runtime, Predefined::getSymbolID(Predefined::values)));
    runtime.ignoreAllocationFailure(
        JSObject::defineOwnProperty(
            proto,
            runtime,
            Predefined::getSymbolID(Predefined::SymbolIterator),
            dpf,
            runtime.makeHandle<NativeFunction>(propValue.getHermesValue())));
  }

  {
    auto propValue = runtime.ignoreAllocationFailure(
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
            Handle<NativeFunction>::vmcast(
                runtime.makeHandle(std::move(propValue)))));
  }

  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::toLocaleString),
      nullptr,
      typedArrayPrototypeToLocaleString,
      0);

  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::toReversed),
      nullptr,
      typedArrayPrototypeToReversed,
      0);

  // TypedArrayBase.xxx
  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::from),
      nullptr,
      typedArrayFrom,
      1);

  defineMethod(
      runtime,
      cons,
      Predefined::getSymbolID(Predefined::of),
      nullptr,
      typedArrayOf,
      0);

  return cons;
}

#define TYPED_ARRAY(name, type)                                       \
  Handle<JSObject> create##name##ArrayConstructor(Runtime &runtime) { \
    return createTypedArrayConstructor<                               \
        type,                                                         \
        CellKind::name##ArrayKind,                                    \
        name##ArrayConstructor>(runtime);                             \
  }
#include "hermes/VM/TypedArrays.def"
#undef TYPED_ARRAY

} // namespace vm
} // namespace hermes

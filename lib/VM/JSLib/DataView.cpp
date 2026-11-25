/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSLibInternal.h"

#include "hermes/VM/BigIntPrimitive.h"
#include "hermes/VM/JSDataView.h"
#include "hermes/VM/JSTypedArray.h"
#include "hermes/VM/StringPrimitive.h"

namespace hermes {
namespace vm {

/// @name DataView.prototype
/// @{

// ES6 24.2.4.1
CallResult<HermesValue> dataViewPrototypeBuffer(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto self = args.dyncastThis<JSDataView>();
  if (!self) {
    return runtime.raiseTypeError(
        "DataView.prototype.buffer called on a non DataView object");
  }
  return HermesValue::encodeObjectValue(self->getBuffer(runtime));
}

// ES6 24.2.4.2
CallResult<HermesValue> dataViewPrototypeByteLength(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto self = args.dyncastThis<JSDataView>();
  if (!self) {
    return runtime.raiseTypeError(
        "DataView.prototype.byteLength called on a non DataView object");
  }
  if (!self->attached(runtime)) {
    return runtime.raiseTypeError(
        "DataView.prototype.byteLength called on a detached ArrayBuffer");
  }
  return HermesValue::encodeTrustedNumberValue(self->byteLength());
}

// ES6 24.2.4.3
CallResult<HermesValue> dataViewPrototypeByteOffset(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto self = args.dyncastThis<JSDataView>();
  if (!self) {
    return runtime.raiseTypeError(
        "DataView.prototype.byteOffset called on a non DataView object");
  }
  if (!self->attached(runtime)) {
    return runtime.raiseTypeError(
        "DataView.prototype.byteOffset called on a detached ArrayBuffer");
  }
  return HermesValue::encodeTrustedNumberValue(self->byteOffset());
}

// ES6 24.2.4.5 - 22.2.4.20 && ES 2018 24.3.1.1
namespace {
template <typename T>
CallResult<HermesValue> dataViewPrototypeGetEncoder(Runtime &, T value) {
  return HermesValue::encodeUntrustedNumberValue(value);
}

CallResult<HermesValue> dataViewPrototypeGetEncoder(
    Runtime &runtime,
    int64_t value) {
  return BigIntPrimitive::fromSigned(runtime, value);
}

CallResult<HermesValue> dataViewPrototypeGetEncoder(
    Runtime &runtime,
    uint64_t value) {
  return BigIntPrimitive::fromUnsigned(runtime, value);
}

template <typename T, CellKind C>
CallResult<HermesValue> dataViewPrototypeGet(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto self = args.dyncastThis<JSDataView>();
  if (!self) {
    return runtime.raiseTypeError(
        "DataView.prototype.get<Type>() called on a non DataView object");
  }
  auto res = toIndex(runtime, args.getArgHandle(0));
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto byteOffset = res->getNumberAs<uint64_t>();
  auto littleEndian = toBoolean(args.getArg(1));
  if (!self->attached(runtime)) {
    return runtime.raiseTypeError(
        "DataView.prototype.get<Type>() called on a detached ArrayBuffer");
  }
  if (byteOffset + sizeof(T) > self->byteLength()) {
    return runtime.raiseRangeError(
        "DataView.prototype.get<Type>(): Cannot "
        "read that many bytes");
  }
  T rawValue = self->get<T>(runtime, byteOffset, littleEndian);
  if constexpr (C == CellKind::Float16ArrayKind) {
    return HermesValue::encodeUntrustedNumberValue(float16ToDouble(rawValue));
  } else {
    return dataViewPrototypeGetEncoder(runtime, rawValue);
  }
}

template <CellKind C>
constexpr bool isBigIntCellKind() {
  return C == CellKind::BigInt64ArrayKind || C == CellKind::BigUint64ArrayKind;
}

template <typename T, CellKind C>
CallResult<HermesValue> dataViewPrototypeSet(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto self = args.dyncastThis<JSDataView>();
  if (!self) {
    return runtime.raiseTypeError(
        "DataView.prototype.set<Type>() called on a non DataView object");
  }
  auto res = toIndex(runtime, args.getArgHandle(0));
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto byteOffset = res->getNumberAs<uint64_t>();
  auto littleEndian = toBoolean(args.getArg(2));
  if constexpr (isBigIntCellKind<C>()) {
    res = toBigInt_RJS(runtime, args.getArgHandle(1));
  } else {
    res = toNumber_RJS(runtime, args.getArgHandle(1));
  }
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  if (!self->attached(runtime)) {
    return runtime.raiseTypeError(
        "DataView.prototype.set<Type> called on a detached ArrayBuffer");
  }
  T value = JSTypedArray<T, C>::toDestType(*res);
  if (byteOffset + sizeof(T) > self->byteLength()) {
    return runtime.raiseRangeError(
        "DataView.prototype.set<Type>(): Cannot "
        "write that many bytes");
  }
  self->set<T>(runtime, byteOffset, value, littleEndian);
  return HermesValue::encodeUndefinedValue();
}
} // namespace

#define TYPED_ARRAY(name, type)                                                \
  CallResult<HermesValue> dataViewPrototypeGet##name(void *ctx, Runtime &rt) { \
    return dataViewPrototypeGet<type, CellKind::name##ArrayKind>(ctx, rt);     \
  }                                                                            \
  CallResult<HermesValue> dataViewPrototypeSet##name(void *ctx, Runtime &rt) { \
    return dataViewPrototypeSet<type, CellKind::name##ArrayKind>(ctx, rt);     \
  }
#define TYPED_ARRAY_NO_CLAMP
#include "hermes/VM/TypedArrays.def"
#undef TYPED_ARRAY_NO_CLAMP
#undef TYPED_ARRAY

/// @}

// ES 2018 24.3.2.1
CallResult<HermesValue> dataViewConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. If NewTarget is undefined, throw a TypeError exception.
  if (!args.isConstructorCall()) {
    return runtime.raiseTypeError(
        "DataView() called in function context instead of constructor");
  }
  struct : public Locals {
    PinnedValue<JSObject> selfParent;
    PinnedValue<JSDataView> self;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  if (LLVM_LIKELY(
          args.getNewTarget().getRaw() ==
          runtime.dataViewConstructor.getHermesValue().getRaw())) {
    lv.selfParent = runtime.dataViewPrototype;
  } else {
    CallResult<PseudoHandle<JSObject>> thisParentRes =
        NativeConstructor::parentForNewThis_RJS(
            runtime,
            Handle<Callable>::vmcast(&args.getNewTarget()),
            runtime.dataViewPrototype);
    if (LLVM_UNLIKELY(thisParentRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.selfParent = std::move(*thisParentRes);
  }
  lv.self = JSDataView::create(runtime, lv.selfParent);

  auto buffer = args.dyncastArg<JSArrayBuffer>(0);
  auto byteLength = args.getArgHandle(2);
  // 2. If Type(buffer) is not Object, throw a TypeError exception
  // 3. If buffer does not have an [[ArrayBufferData]] internal slot, throw a
  // TypeError exception.
  if (!buffer) {
    return runtime.raiseTypeError(
        "new DataView(buffer, [byteOffset], [byteLength]): "
        "buffer must be an ArrayBuffer");
  }

  // 4. Let offset be ToIndex(byteOffset).
  auto res = toIndex(runtime, args.getArgHandle(1));
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto offset = res->getNumberAs<uint64_t>();
  // ES2025 25.3.2.1 Step 4. If IsDetachedBuffer(buffer) is true, throw a
  // TypeError exception.
  if (!buffer->attached()) {
    return runtime.raiseTypeError(
        "Cannot construct DataView on a detached ArrayBuffer");
  }
  // 6. Let bufferByteLength be buffer.[[ArrayBufferByteLength]].
  auto bufferByteLength = buffer->size();
  // 7. If offset > bufferByteLength, throw a RangeError exception.
  if (offset > bufferByteLength) {
    return runtime.raiseRangeError(
        "new DataView(buffer, [byteOffset], "
        "[byteLength]): byteOffset must be <= the "
        "buffer's byte length");
  }
  double viewByteLength = 0;
  // 8. If byteLength is either not present or undefined, then
  if (byteLength->isUndefined()) {
    // 8a. Let viewByteLength be bufferByteLength - offset.
    viewByteLength = bufferByteLength - offset;
  } else {
    // 9a. Let viewByteLength be ? ToIndex(byteLength).
    auto res2 = toIndex(runtime, byteLength);
    if (res2 == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    viewByteLength = res2->getNumber();
    // 9b. If offset+viewByteLength > bufferByteLength, throw a RangeError
    // exception.
    if (offset + viewByteLength > bufferByteLength) {
      return runtime.raiseRangeError(
          "new DataView(buffer, [byteOffset], "
          "[byteLength]): byteOffset + byteLength must be "
          "<= the length of the buffer");
    }
  }
  // ES2025 25.3.2.1 Step 11. If IsDetachedBuffer(buffer) is true, throw a
  // TypeError exception.
  if (!buffer->attached()) {
    return runtime.raiseTypeError(
        "Cannot construct DataView on a detached ArrayBuffer");
  }
  lv.self->setBuffer(runtime, *buffer, offset, viewByteLength);
  return lv.self.getHermesValue();
}

HermesValue createDataViewConstructor(Runtime &runtime) {
  auto proto = Handle<JSObject>::vmcast(&runtime.dataViewPrototype);

  struct : public Locals {
    PinnedValue<NativeConstructor> cons;
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  defineSystemConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::DataView),
      dataViewConstructor,
      proto,
      1,
      lv.cons);

  // DataView.prototype.xxx() methods.
  defineAccessor(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::buffer),
      nullptr,
      dataViewPrototypeBuffer,
      nullptr,
      false,
      true);
  defineAccessor(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::byteLength),
      nullptr,
      dataViewPrototypeByteLength,
      nullptr,
      false,
      true);
  defineAccessor(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::byteOffset),
      nullptr,
      dataViewPrototypeByteOffset,
      nullptr,
      false,
      true);

#define TYPED_ARRAY(name, type)                       \
  defineMethod(                                       \
      runtime,                                        \
      proto,                                          \
      Predefined::getSymbolID(Predefined::get##name), \
      nullptr,                                        \
      dataViewPrototypeGet##name,                     \
      1);                                             \
  defineMethod(                                       \
      runtime,                                        \
      proto,                                          \
      Predefined::getSymbolID(Predefined::set##name), \
      nullptr,                                        \
      dataViewPrototypeSet##name,                     \
      2);
#define TYPED_ARRAY_NO_CLAMP
#include "hermes/VM/TypedArrays.def"

  auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  lv.str = runtime.getPredefinedString(Predefined::DataView);
  defineProperty(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      lv.str,
      dpf);

  // DataView.xxx() methods.

  return lv.cons.getHermesValue();
}

} // namespace vm
} // namespace hermes

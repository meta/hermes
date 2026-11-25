/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES6 24.1 ArrayBuffer
//===----------------------------------------------------------------------===//
#include "JSLibInternal.h"

#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/JSDataView.h"
#include "hermes/VM/JSTypedArray.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/StringPrimitive.h"

namespace hermes {
namespace vm {

using std::max;
using std::min;

/// @name Implementation
/// @{

HermesValue createArrayBufferConstructor(Runtime &runtime) {
  auto arrayBufferPrototype =
      Handle<JSObject>::vmcast(&runtime.arrayBufferPrototype);

  struct : public Locals {
    PinnedValue<NativeConstructor> cons;
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  defineSystemConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::ArrayBuffer),
      arrayBufferConstructor,
      arrayBufferPrototype,
      1,
      lv.cons);

  // ArrayBuffer.prototype.xxx() methods.
  defineAccessor(
      runtime,
      arrayBufferPrototype,
      Predefined::getSymbolID(Predefined::byteLength),
      nullptr,
      arrayBufferPrototypeByteLength,
      nullptr,
      false,
      true);
  defineAccessor(
      runtime,
      arrayBufferPrototype,
      Predefined::getSymbolID(Predefined::detached),
      nullptr,
      arrayBufferPrototypeDetached,
      nullptr,
      false,
      true);
  defineMethod(
      runtime,
      arrayBufferPrototype,
      Predefined::getSymbolID(Predefined::slice),
      nullptr,
      arrayBufferPrototypeSlice,
      2);

  auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  lv.str = runtime.getPredefinedString(Predefined::ArrayBuffer);
  defineProperty(
      runtime,
      arrayBufferPrototype,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      lv.str,
      dpf);

  // ArrayBuffer.xxx() methods.
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::isView),
      nullptr,
      arrayBufferIsView,
      1);

  return lv.cons.getHermesValue();
}

CallResult<HermesValue> arrayBufferConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. If NewTarget is undefined, throw a TypeError exception.
  if (!args.isConstructorCall()) {
    return runtime.raiseTypeError(
        "ArrayBuffer() called in function context instead of constructor");
  }
  struct : public Locals {
    PinnedValue<JSObject> selfParent;
    PinnedValue<JSArrayBuffer> self;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  if (LLVM_LIKELY(
          args.getNewTarget().getRaw() ==
          runtime.arrayBufferConstructor.getHermesValue().getRaw())) {
    lv.selfParent = runtime.arrayBufferPrototype;
  } else {
    CallResult<PseudoHandle<JSObject>> thisParentRes =
        NativeConstructor::parentForNewThis_RJS(
            runtime,
            Handle<Callable>::vmcast(&args.getNewTarget()),
            runtime.arrayBufferPrototype);
    if (LLVM_UNLIKELY(thisParentRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.selfParent = std::move(*thisParentRes);
  }
  lv.self = JSArrayBuffer::create(runtime, lv.selfParent);

  auto length = args.getArgHandle(0);

  // 2. Let byteLength be ToIndex(length).
  auto res = toIndex(runtime, length);
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t byteLength = res->getNumberAs<uint64_t>();

  // 3. Return AllocateArrayBuffer(NewTarget, byteLength).
  // This object should not have been initialized yet, ensure this is true
  assert(
      !lv.self->attached() &&
      "A new array buffer should not have an existing buffer");
  if (byteLength > std::numeric_limits<JSArrayBuffer::size_type>::max()) {
    // On a non-64-bit platform and requested a buffer size greater than
    // this platform's size type can hold
    return runtime.raiseRangeError("Too large of a byteLength requested");
  }
  if (JSArrayBuffer::createDataBlock(runtime, lv.self, byteLength) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return lv.self.getHermesValue();
}

CallResult<HermesValue> arrayBufferIsView(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. If Type(arg) is not Object, return false.
  // 2. If arg has a [[ViewedArrayBuffer]] internal slot, return true.
  // 3. Return false.
  return HermesValue::encodeBoolValue(
      vmisa<JSTypedArrayBase>(args.getArg(0)) ||
      vmisa<JSDataView>(args.getArg(0)));
}

CallResult<HermesValue> arrayBufferPrototypeByteLength(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto self = args.dyncastThis<JSArrayBuffer>();
  if (!self) {
    return runtime.raiseTypeError(
        "byteLength called on a non ArrayBuffer object");
  }
  return HermesValue::encodeTrustedNumberValue(self->size());
}

CallResult<HermesValue> arrayBufferPrototypeDetached(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto self = args.dyncastThis<JSArrayBuffer>();
  if (!self) {
    return runtime.raiseTypeError(
        "detached called on a non ArrayBuffer object");
  }
  return HermesValue::encodeBoolValue(!self->attached());
}

CallResult<HermesValue> arrayBufferPrototypeSlice(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto start = args.getArgHandle(0);
  auto end = args.getArgHandle(1);
  // 1. Let O be the this value.
  auto self = args.dyncastThis<JSArrayBuffer>();
  // 2. If Type(O) is not Object, throw a TypeError exception.
  // 3. If O does not have an [[ArrayBufferData]] internal slot, throw a
  // TypeError exception. 4. If IsDetachedBuffer(O) is true, throw a TypeError
  // exception.
  if (!self) {
    return runtime.raiseTypeError(
        "Called ArrayBuffer.prototype.slice on a non-ArrayBuffer");
  }

  struct : public Locals {
    PinnedValue<JSArrayBuffer> newBuf;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 5. Let len be the value of O’s [[ArrayBufferByteLength]] internal slot.
  double len = self->size();
  // 6. Let relativeStart be ToIntegerOrInfinity(start).
  auto intRes = toIntegerOrInfinity(runtime, start);
  if (intRes == ExecutionStatus::EXCEPTION) {
    // 7. ReturnIfAbrupt(relativeStart).
    return ExecutionStatus::EXCEPTION;
  }
  double relativeStart = intRes->getNumber();
  // 8. If relativeStart < 0, let first be max((len + relativeStart),0); else
  // let first be min(relativeStart, len).
  double first = relativeStart < 0 ? max(len + relativeStart, 0.0)
                                   : min(relativeStart, len);
  // 9. If end is undefined, let relativeEnd be len; else let relativeEnd be
  // ToIntegerOrInfinity(end).
  double relativeEnd;
  if (end->isUndefined()) {
    relativeEnd = len;
  } else {
    intRes = toIntegerOrInfinity(runtime, end);
    if (intRes == ExecutionStatus::EXCEPTION) {
      // 10. ReturnIfAbrupt(relativeEnd).
      return ExecutionStatus::EXCEPTION;
    }
    relativeEnd = intRes->getNumber();
  }
  // 11. If relativeEnd < 0, let final be max((len + relativeEnd),0); else let
  // final be min(relativeEnd, len).
  // NOTE: final is a keyword
  double finale =
      relativeEnd < 0 ? max(len + relativeEnd, 0.0) : min(relativeEnd, len);
  // 12. Let newLen be max(final-first,0).
  double newLen = max(finale - first, 0.0);
  // Reduce the doubles to integers for addition and creation
  JSArrayBuffer::size_type first_int = first;
  JSArrayBuffer::size_type newLen_int = newLen;
  // 13. Let ctor be SpeciesConstructor(O, %ArrayBuffer%).
  // 14. ReturnIfAbrupt(ctor).
  // 15. Let new be Construct(ctor, «newLen»).
  // 16. ReturnIfAbrupt(new).

  lv.newBuf.castAndSetHermesValue<JSArrayBuffer>(
      JSArrayBuffer::create(
          runtime, Handle<JSObject>::vmcast(&runtime.arrayBufferPrototype))
          .getHermesValue());

  if (JSArrayBuffer::createDataBlock(runtime, lv.newBuf, newLen_int) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // 17. If new does not have an [[ArrayBufferData]] internal slot, throw a
  // TypeError exception.
  // 18. If IsDetachedBuffer(new) is true, throw a TypeError exception.
  // 19. If SameValue(new, O) is true, throw a TypeError exception.
  // 20. If the value of new’s [[ArrayBufferByteLength]] internal
  // slot < newLen, throw a TypeError exception.
  // 21. NOTE: Side-effects of the above steps may have detached O.
  // 22. If IsDetachedBuffer(O) is true, throw a TypeError exception.
  if (!self->attached() || !lv.newBuf->attached()) {
    return runtime.raiseTypeError("Cannot split with detached ArrayBuffers");
  }

  // 23. Let fromBuf be the value of O’s [[ArrayBufferData]] internal slot.
  // 24. Let toBuf be the value of new’s [[ArrayBufferData]] internal slot.
  // 25. Perform CopyDataBlockBytes(toBuf, 0, fromBuf, first, newLen).
  JSArrayBuffer::copyDataBlockBytes(
      runtime, *lv.newBuf, 0, *self, first_int, newLen_int);
  // 26. Return new.
  return lv.newBuf.getHermesValue();
}
/// @}
} // namespace vm
} // namespace hermes

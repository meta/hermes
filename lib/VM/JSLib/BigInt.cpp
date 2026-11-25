/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// The BigInt constructor (https://tc39.es/ecma262/#sec-bigint-constructor).
//===----------------------------------------------------------------------===//
#include "JSLibInternal.h"

#include "hermes/VM/BigIntPrimitive.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/PrimitiveBox.h"

namespace hermes {
namespace vm {

void createBigIntConstructor(Runtime &runtime) {
  Handle<JSObject> bigintPrototype{runtime.bigintPrototype};

  struct : public Locals {
    PinnedValue<NativeConstructor> cons;
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  defineSystemConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::BigInt),
      bigintConstructor,
      bigintPrototype,
      1,
      lv.cons);

  // BigInt.prototype.xxx methods.
  // https://tc39.es/ecma262/#sec-properties-of-the-bigint-prototype-object
  void *ctx = nullptr;
  defineMethod(
      runtime,
      bigintPrototype,
      Predefined::getSymbolID(Predefined::toString),
      ctx,
      bigintPrototypeToString,
      0);
  defineMethod(
      runtime,
      bigintPrototype,
      Predefined::getSymbolID(Predefined::valueOf),
      ctx,
      bigintPrototypeValueOf,
      0);
  defineMethod(
      runtime,
      bigintPrototype,
      Predefined::getSymbolID(Predefined::toLocaleString),
      ctx,
      bigintPrototypeToLocaleString,
      1);

  // BigInt.xxx() methods.
  // https://tc39.es/ecma262/#sec-properties-of-the-bigint-constructor
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::asIntN),
      reinterpret_cast<void *>(&BigIntPrimitive::asIntN),
      bigintTruncate,
      2);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::asUintN),
      reinterpret_cast<void *>(&BigIntPrimitive::asUintN),
      bigintTruncate,
      2);

  // BigInt.xxx properties
  auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  lv.str = runtime.getPredefinedString(Predefined::BigInt);
  defineProperty(
      runtime,
      bigintPrototype,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      lv.str,
      dpf);
}

CallResult<HermesValue> bigintConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // The bigint constructor is not a constructor according to
  // https://262.ecma-international.org/#sec-bigint-constructor
  if (args.isConstructorCall()) {
    return runtime.raiseTypeError("BigInt is not a constructor");
  }

  struct : public Locals {
    PinnedValue<> hArg0;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.hArg0 = args.getArg(0);
  auto prim = toPrimitive_RJS(runtime, lv.hArg0, PreferredType::NUMBER);
  if (LLVM_UNLIKELY(prim == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (prim->isNumber()) {
    return numberToBigInt(runtime, prim->getNumber());
  }

  return toBigInt_RJS(runtime, lv.hArg0);
}

CallResult<HermesValue> bigintPrototypeToLocaleString(
    void *ctx,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto bigint = thisBigIntValue(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(bigint == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  struct : public Locals {
    PinnedValue<> bigintHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Call toString, as JSC does.
  // TODO(T120187933): Format string according to locale.
  lv.bigintHandle = *bigint;
  auto res = toString_RJS(runtime, lv.bigintHandle);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return res->getHermesValue();
}

CallResult<HermesValue> bigintPrototypeToString(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. Let x be ? thisBigIntValue(this value).
  auto x = thisBigIntValue(runtime, args.getThisHandle());

  if (LLVM_UNLIKELY(x == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  struct : public Locals {
    PinnedValue<BigIntPrimitive> xHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.xHandle.castAndSetHermesValue<BigIntPrimitive>(*x);

  // 2. If radix is undefined, let radixMV be 10.
  uint32_t radixMV = 10;

  // 3. Else, let radixMV be ? ToIntegerOrInfinity(radix).
  auto radixArg = args.getArgHandle(0);
  if (!radixArg->isUndefined()) {
    auto r = toIntegerOrInfinity(runtime, radixArg);
    if (LLVM_UNLIKELY(r == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    assert(r->isNumber());
    // 4. If radixMV < 2 or radixMV > 36, throw a RangeError exception.
    if (r->getNumber() < 2 || r->getNumber() > 36) {
      return runtime.raiseRangeError(
          "radix out-of-range in BigInt.prototype.toString");
    }

    radixMV = r->getNumber();
  }

  // 5. If radixMV = 10, return ! ToString(x).
  // 6. Return the String representation of x.
  return BigIntPrimitive::toString(
      runtime, PseudoHandle<BigIntPrimitive>{lv.xHandle}, radixMV);
}

CallResult<HermesValue> bigintPrototypeValueOf(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return thisBigIntValue(runtime, args.getThisHandle());
}

using TruncateOp =
    CallResult<HermesValue> (*)(Runtime &, uint64_t, Handle<BigIntPrimitive>);

CallResult<HermesValue> bigintTruncate(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto bitsRes = toIndex(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(bitsRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t bits = bitsRes->getNumberAs<uint64_t>();

  auto bigint = toBigInt_RJS(runtime, args.getArgHandle(1));
  if (LLVM_UNLIKELY(bigint == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  struct : public Locals {
    PinnedValue<BigIntPrimitive> bigintHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.bigintHandle.castAndSetHermesValue<BigIntPrimitive>(*bigint);
  auto op = reinterpret_cast<TruncateOp>(ctx);
  return (*op)(runtime, bits, lv.bigintHandle);
}

} // namespace vm
} // namespace hermes

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES5.1 15.7 Initialize the Number constructor.
//===----------------------------------------------------------------------===//

#include "JSLibInternal.h"

#include "hermes/VM/Operations.h"
#include "hermes/VM/PrimitiveBox.h"
#include "hermes/VM/SmallXString.h"
#include "hermes/VM/StringPrimitive.h"

#include "dtoa/dtoa.h"

#include "llvh/ADT/SmallString.h"
#include "llvh/Support/Format.h"

namespace hermes {
namespace vm {

HermesValue createNumberConstructor(Runtime &runtime) {
  Handle<JSNumber> numberPrototype{runtime.numberPrototype};

  defineMethod(
      runtime,
      numberPrototype,
      Predefined::getSymbolID(Predefined::valueOf),
      nullptr,
      numberPrototypeValueOf,
      0);
  defineMethod(
      runtime,
      numberPrototype,
      Predefined::getSymbolID(Predefined::toString),
      nullptr,
      numberPrototypeToString,
      1);
  defineMethod(
      runtime,
      numberPrototype,
      Predefined::getSymbolID(Predefined::toLocaleString),
      nullptr,
      numberPrototypeToLocaleString,
      0);
  defineMethod(
      runtime,
      numberPrototype,
      Predefined::getSymbolID(Predefined::toFixed),
      nullptr,
      numberPrototypeToFixed,
      1);
  defineMethod(
      runtime,
      numberPrototype,
      Predefined::getSymbolID(Predefined::toExponential),
      nullptr,
      numberPrototypeToExponential,
      1);
  defineMethod(
      runtime,
      numberPrototype,
      Predefined::getSymbolID(Predefined::toPrecision),
      nullptr,
      numberPrototypeToPrecision,
      1);

  DefinePropertyFlags constantDPF =
      DefinePropertyFlags::getDefaultNewPropertyFlags();
  constantDPF.enumerable = 0;
  constantDPF.writable = 0;
  constantDPF.configurable = 0;

  struct : public Locals {
    PinnedValue<> numberValueHandle;
    PinnedValue<NativeConstructor> cons;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  defineSystemConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::Number),
      numberConstructor,
      numberPrototype,
      1,
      lv.cons);

  auto setNumberValueProperty = [&](SymbolID name, double value) {
    lv.numberValueHandle = HermesValue::encodeTrustedNumberValue(value);
    auto result = JSObject::defineOwnProperty(
        lv.cons, runtime, name, constantDPF, lv.numberValueHandle);
    assert(
        result != ExecutionStatus::EXCEPTION &&
        "defineOwnProperty() failed on a new object");
    (void)result;
  };

  setNumberValueProperty(
      Predefined::getSymbolID(Predefined::MAX_VALUE),
      std::numeric_limits<double>::max());
  setNumberValueProperty(
      Predefined::getSymbolID(Predefined::MIN_VALUE),
      std::numeric_limits<double>::denorm_min());
  setNumberValueProperty(
      Predefined::getSymbolID(Predefined::NaN),
      std::numeric_limits<double>::quiet_NaN());
  setNumberValueProperty(
      Predefined::getSymbolID(Predefined::NEGATIVE_INFINITY),
      -std::numeric_limits<double>::infinity());
  setNumberValueProperty(
      Predefined::getSymbolID(Predefined::POSITIVE_INFINITY),
      std::numeric_limits<double>::infinity());
  // ES6.0 20.1.2.1
  setNumberValueProperty(
      Predefined::getSymbolID(Predefined::EPSILON),
      std::numeric_limits<double>::epsilon());
  // ES6.0 20.1.2.6
  constexpr double kMaxSafeInteger = 9007199254740991;
  setNumberValueProperty(
      Predefined::getSymbolID(Predefined::MAX_SAFE_INTEGER), kMaxSafeInteger);
  // ES6.0 20.1.2.8
  setNumberValueProperty(
      Predefined::getSymbolID(Predefined::MIN_SAFE_INTEGER), -kMaxSafeInteger);

  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::isFinite),
      nullptr,
      numberIsFinite,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::isInteger),
      nullptr,
      numberIsInteger,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::isNaN),
      nullptr,
      numberIsNaN,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::isSafeInteger),
      nullptr,
      numberIsSafeInteger,
      1);
  defineProperty(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::parseInt),
      runtime.parseIntFunction);
  defineProperty(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::parseFloat),
      runtime.parseFloatFunction);
  return lv.cons.getHermesValue();
}

CallResult<HermesValue> numberConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  double value = +0.0;

  if (args.getArgCount() > 0) {
    CallResult<HermesValue> res = toNumeric_RJS(runtime, args.getArgHandle(0));
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (!res->isBigInt()) {
      value = res->getNumber();
    } else {
      CallResult<double> val = res->getBigInt()->toDouble(runtime);
      if (LLVM_UNLIKELY(val == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      value = *val;
    }
  }

  if (!args.isConstructorCall()) {
    return HermesValue::encodeTrustedNumberValue(value);
  }

  if (LLVM_LIKELY(
          args.getNewTarget().getRaw() ==
          runtime.numberConstructor.getHermesValue().getRaw())) {
    return JSNumber::create(runtime, value, runtime.numberPrototype)
        .getHermesValue();
  }
  struct : public Locals {
    PinnedValue<JSObject> selfParent;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  CallResult<PseudoHandle<JSObject>> thisParentRes =
      NativeConstructor::parentForNewThis_RJS(
          runtime,
          Handle<Callable>::vmcast(&args.getNewTarget()),
          runtime.numberPrototype);
  if (LLVM_UNLIKELY(thisParentRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.selfParent = std::move(*thisParentRes);
  return JSNumber::create(runtime, value, lv.selfParent).getHermesValue();
}

CallResult<HermesValue> numberIsFinite(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (!args.getArg(0).isNumber()) {
    // If Type(number) is not Number, return false.
    return HermesValue::encodeBoolValue(false);
  }
  double number = args.getArg(0).getNumber();
  // If number is NaN, +∞, or −∞, return false.
  // Otherwise, return true.
  return HermesValue::encodeBoolValue(std::isfinite(number));
}

CallResult<HermesValue> numberIsInteger(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (!args.getArg(0).isNumber()) {
    // If Type(number) is not Number, return false.
    return HermesValue::encodeBoolValue(false);
  }
  double number = args.getArg(0).getNumber();

  if (!std::isfinite(number)) {
    // If number is NaN, +∞, or −∞, return false.
    return HermesValue::encodeBoolValue(false);
  }
  // Let integer be ToIntegerOrInfinity(number).
  assert(!std::isnan(number) && "number must not be NaN after the check");
  // Call std::trunc because we've alredy checked NaN with isfinite.
  double integer = std::trunc(number);

  // If integer is not equal to number, return false.
  // Otherwise, return true.
  return HermesValue::encodeBoolValue(integer == number);
}

CallResult<HermesValue> numberIsNaN(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (!args.getArg(0).isNumber()) {
    // If Type(number) is not Number, return false.
    return HermesValue::encodeBoolValue(false);
  }
  double number = args.getArg(0).getNumber();
  // If number is NaN, return true.
  // Otherwise, return false.
  return HermesValue::encodeBoolValue(std::isnan(number));
}

CallResult<HermesValue> numberIsSafeInteger(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (!args.getArg(0).isNumber()) {
    // If Type(number) is not Number, return false.
    return HermesValue::encodeBoolValue(false);
  }
  double number = args.getArg(0).getNumber();

  if (!std::isfinite(number)) {
    // If number is NaN, +∞, or −∞, return false.
    return HermesValue::encodeBoolValue(false);
  }

  // Let integer be ToIntegerOrInfinity(number).
  assert(!std::isnan(number) && "number must not be NaN after the check");
  // Call std::trunc because we've alredy checked NaN with isfinite.
  double integer = std::trunc(number);

  if (integer != number) {
    // If integer is not equal to number, return false.
    return HermesValue::encodeBoolValue(false);
  }

  // If abs(integer) ≤ 2^53−1, return true.
  // Otherwise, return false.
  return HermesValue::encodeBoolValue(
      std::abs(integer) <= ((double)((uint64_t)1 << 53)) - 1);
}

CallResult<HermesValue> numberPrototypeValueOf(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (args.getThisArg().isNumber()) {
    return args.getThisArg();
  }
  auto *numPtr = dyn_vmcast<JSNumber>(args.getThisArg());
  if (!numPtr) {
    return runtime.raiseTypeError(
        "Number.prototype.valueOf() can only be used on Number");
  }
  return HermesValue::encodeTrustedNumberValue(numPtr->getPrimitiveNumber());
}

CallResult<HermesValue> numberPrototypeToString(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  const size_t MIN_RADIX = 2;
  const size_t MAX_RADIX = 36;

  unsigned radix;
  double number;

  // Extract the number from this.
  if (args.getThisArg().isNumber()) {
    number = args.getThisArg().getNumber();
  } else {
    auto *numPtr = dyn_vmcast<JSNumber>(args.getThisArg());
    if (!numPtr) {
      return runtime.raiseTypeError(
          "Number.prototype.toString() can only be used on Number");
    }
    number = numPtr->getPrimitiveNumber();
  }

  if (args.getArg(0).isUndefined())
    radix = 10;
  else {
    // ToIntegerOrInfinity(arg0).
    auto intRes = toIntegerOrInfinity(runtime, args.getArgHandle(0));
    if (intRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    auto d = intRes->getNumber();
    if (d < MIN_RADIX || d > MAX_RADIX) {
      return runtime.raiseRangeError("Invalid radix value");
    }
    radix = (unsigned)d;
  }

  // Directly output finite numbers in a non-decimal base.
  // Note that this is implementation defined behavior: we output a rounded
  // string such that we're as close as possible to the actual precision encoded
  // in the double value given by number.
  if (radix != 10 && std::isfinite(number)) {
    return numberToStringWithRadix(runtime, number, radix).getHermesValue();
  }

  // Radix 10 and non-finite values simply call toString.
  struct : public Locals {
    PinnedValue<> numberHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.numberHandle = HermesValue::encodeTrustedNumberValue(number);
  auto resultRes = toString_RJS(runtime, lv.numberHandle);
  if (LLVM_UNLIKELY(resultRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return resultRes->getHermesValue();
}

CallResult<HermesValue> numberPrototypeToLocaleString(
    void *ctx,
    Runtime &runtime) {
#ifdef HERMES_ENABLE_INTL
  return intlNumberPrototypeToLocaleString(/* unused */ ctx, runtime);
#else
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  double number;

  // Extract the number from this.
  if (args.getThisArg().isNumber()) {
    number = args.getThisArg().getNumber();
  } else {
    auto *numPtr = dyn_vmcast<JSNumber>(args.getThisArg());
    if (!numPtr) {
      return runtime.raiseTypeError(
          "Number.prototype.toLocaleString() can only be used on Number");
    }
    number = numPtr->getPrimitiveNumber();
  }

  // Call toString, as JSC does.
  // TODO: Format string according to locale.
  struct : public Locals {
    PinnedValue<> numberHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.numberHandle = HermesValue::encodeTrustedNumberValue(number);
  auto res = toString_RJS(runtime, lv.numberHandle);
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  return res->getHermesValue();
#endif
}

CallResult<HermesValue> numberPrototypeToFixed(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // The number to make a string toFixed.
  double x;
  if (args.getThisArg().isNumber()) {
    x = args.getThisArg().getNumber();
  } else {
    auto numPtr = Handle<JSNumber>::dyn_vmcast(args.getThisHandle());
    if (LLVM_UNLIKELY(!numPtr)) {
      return runtime.raiseTypeError(
          "Number.prototype.toFixed() can only be used on Number");
    }
    x = numPtr->getPrimitiveNumber();
  }

  auto intRes = toIntegerOrInfinity(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double fDouble = intRes->getNumber();

  // 3. If f < 0 or f > 100, throw a RangeError exception.
  if (LLVM_UNLIKELY(fDouble < 0 || fDouble > 100)) {
    return runtime.raiseRangeError(
        "toFixed argument must be between 0 and 100");
  }
  /// Number of digits after the decimal point.
  /// Because we checked, 0 <= f <= 20.
  /// In particular, we know that f is non-negative.
  int32_t f = static_cast<int32_t>(fDouble);

  if (std::isnan(x)) {
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::NaN));
  }

  // Account for very large numbers.
  if (std::abs(x) >= 1e21) {
    // toString(x) if abs(x) >= 10^21.
    struct : public Locals {
      PinnedValue<> xHandle;
    } lv;
    LocalsRAII lraii(runtime, &lv);
    lv.xHandle = HermesValue::encodeTrustedNumberValue(x);
    auto resultRes = toString_RJS(runtime, lv.xHandle);
    if (LLVM_UNLIKELY(resultRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return resultRes->getHermesValue();
  }

  // If negative, set a flag, proceed as if positive, and add a '-' later.
  bool negative = false;
  if (x < 0) {
    negative = true;
    x = -x;
  }

  // Decimal point index.
  int decPt;

  // 1 if negative, 0 else.
  int sign;

  // Points to the end of the string s after it's populated.
  char *sEnd;

  // 0 <= x < 10e21 now.
  // Let n be an integer such that n/(10^f) - x is close to 0.
  // Store the string representation of n, as provided by dtoa.
  // Use mode=3 and precision=f for the dtoa call (fixed precision dtoa).
  llvh::SmallString<32> n;
  {
    DtoaAllocator<> dalloc{};
    char *s = ::dtoa_fixedpoint(dalloc, x, 3, f, &decPt, &sign, &sEnd);
    n.append(s, sEnd);
    ::g_freedtoa(dalloc, s);
  }

  // Minimum number of digits required by the specified fixed-point length.
  size_t minNLen = decPt + f;

  // Pad n to account for for the specified fixed-point length.
  while (n.size() < minNLen) {
    n.push_back('0');
  }

  // Final string.
  llvh::SmallString<32> m{};

  // Check if n is 0 (n is empty or has no non-'0' characters);
  bool isZero = n.find_first_not_of('0') == llvh::StringRef::npos;

  if (isZero) {
    // n is zero, so just add '0' and be done.
    m.push_back('0');
  } else {
    // n is non-zero, so add all of n to m.
    m.append(n);
  }

  if (f != 0) {
    int32_t k = m.size();
    if (k <= f) {
      // Need leading zeroes after the decimal place if there aren't enough
      // digits after the decimal place.
      // Insert f+1-k occurrences of '0' to ensure we have enough digits.
      for (int32_t i = 0; i < f + 1 - k; ++i) {
        m.insert(m.begin(), '0');
      }
      // Update k to the new size.
      k = f + 1;
    }
    // Place the decimal point after the first k-f characters of m,
    // ensuring we have f digits after the decimal point.
    m.insert(m.begin() + (k - f), '.');
  }

  if (negative) {
    m.insert(m.begin(), '-');
  }

  return StringPrimitive::create(runtime, m);
}

CallResult<HermesValue> numberPrototypeToExponential(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // The number to make a string toExponential.
  double x;
  if (args.getThisArg().isNumber()) {
    x = args.getThisArg().getNumber();
  } else {
    auto numPtr = Handle<JSNumber>::dyn_vmcast(args.getThisHandle());
    if (LLVM_UNLIKELY(!numPtr)) {
      return runtime.raiseTypeError(
          "Number.prototype.toExponential() can only be used on Number");
    }
    x = numPtr->getPrimitiveNumber();
  }

  auto res = toIntegerOrInfinity(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double fDouble = res->getNumber();

  if (std::isnan(x)) {
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::NaN));
  }
  if (x == std::numeric_limits<double>::infinity()) {
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::Infinity));
  }
  if (x == -std::numeric_limits<double>::infinity()) {
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::NegativeInfinity));
  }

  // 8. If f < 0 or f > 100, throw a RangeError exception.
  if (LLVM_UNLIKELY(
          !args.getArg(0).isUndefined() && (fDouble < 0 || fDouble > 100))) {
    return runtime.raiseRangeError(
        "toExponential argument must be between 0 and 100");
  }
  /// Number of digits after the decimal point.
  /// Because we checked, 0 <= f <= 20.
  /// In particular, we know that f is non-negative.
  int f = static_cast<int>(fDouble);

  // If negative, set a flag, proceed as if positive, and add a '-' later.
  bool negative = false;
  if (x < 0) {
    negative = true;
    x = -x;
  }

  // Final result.
  llvh::SmallString<32> n{};

  // The exponent in final string.
  int e;
  if (x == 0) {
    // Implement the spec as in ES6, which most other implementations do.
    // The ES5.1 spec says to output "0" regardless of the provided f value,
    // but ES6 does not.

    // Add trailing 0s to account for the supplied f,
    // allowing for returning, e.g. "0.000e+0".
    for (int i = 0; i < f + 1; ++i) {
      n.push_back('0');
    }
    e = 0;
  } else {
    // x != 0
    // Decimal point index.
    int decPt;
    // 1 if negative, 0 else.
    int sign;
    // Points to the end of the string s after it's populated.
    char *sEnd;

    DtoaAllocator<> dalloc{};
    if (!args.getArg(0).isUndefined()) {
      // Store the string representation of n, as provided by dtoa.
      // Use mode=2 and precision=f+1 for the dtoa call (precision dtoa).
      // Precision is f+1 to account for digit in front of the decimal point.
      char *s = ::dtoa_fixedpoint(dalloc, x, 2, f + 1, &decPt, &sign, &sEnd);
      n.append(s, sEnd);
      ::g_freedtoa(dalloc, s);

      // Minimum length of the string should be enough to account for
      // f digits after the decimal point, and 1 digit before it.
      size_t minNLen = f + 1;
      while (n.size() < minNLen) {
        n.push_back('0');
      }
    } else {
      // Store the string representation of n, as provided by dtoa.
      // We use the default version of dtoa, so we get the shortest string.
      // mode=0 and precision=0 give the shortest string.
      char *s = ::g_dtoa(dalloc, x, 0, 0, &decPt, &sign, &sEnd);
      n.append(s, sEnd);
      ::g_freedtoa(dalloc, s);

      // All but the first digit of n will be after the decimal point.
      f = n.size() - 1;
    }
    // We want the exponent to be one less than the position of the decPt.
    // Example: 123.45 (f=2) has n=12345, decPt=3, so we want e=2.
    e = decPt - 1;
  }

  if (f != 0) {
    // This is valid because there are a non-zero number of digits after the
    // decimal point.
    n.insert(n.begin() + 1, '.');
  }

  // Append the exponent, including the '+' sign if positive.
  if (e == 0) {
    n.append("e+0");
  } else {
    llvh::raw_svector_ostream os{n};
    os << llvh::format("e%+d", e);
  }

  if (negative) {
    n.insert(n.begin(), '-');
  }
  return runtime.ignoreAllocationFailure(StringPrimitive::create(runtime, n));
}

CallResult<HermesValue> numberPrototypeToPrecision(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // The number to make a string toPrecision.
  double x;
  if (args.getThisArg().isNumber()) {
    x = args.getThisArg().getNumber();
  } else {
    auto numPtr = Handle<JSNumber>::dyn_vmcast(args.getThisHandle());
    if (LLVM_UNLIKELY(!numPtr)) {
      return runtime.raiseTypeError(
          "Number.prototype.toPrecision() can only be used on Number");
    }
    x = numPtr->getPrimitiveNumber();
  }

  if (args.getArg(0).isUndefined()) {
    struct : public Locals {
      PinnedValue<> xHandle;
    } lv;
    LocalsRAII lraii(runtime, &lv);
    lv.xHandle = HermesValue::encodeTrustedNumberValue(x);
    auto resultRes = toString_RJS(runtime, lv.xHandle);
    if (LLVM_UNLIKELY(resultRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return resultRes->getHermesValue();
  }

  auto intRes = toIntegerOrInfinity(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double pDouble = intRes->getNumber();

  if (std::isnan(x)) {
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::NaN));
  }
  if (x == std::numeric_limits<double>::infinity()) {
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::Infinity));
  }
  if (x == -std::numeric_limits<double>::infinity()) {
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::NegativeInfinity));
  }

  // 8. If p < 1 or p > 100, throw a RangeError exception.
  if (pDouble < 1 || pDouble > 100) {
    return runtime.raiseRangeError(
        "toPrecision argument must be between 1 and 100");
  }
  /// Number of significant digits in the result.
  /// Because we checked, 1 <= p <= 21.
  /// In particular, we know that p is non-negative.
  int p = static_cast<int>(pDouble);

  // If negative, set a flag, proceed as if positive, and add a '-' later.
  bool negative = false;
  if (x < 0) {
    negative = true;
    x = -x;
  }

  // Final result: add '-' to the start if x was negative.
  llvh::SmallString<32> n{};

  // The exponent in final string.
  int e;
  if (x == 0) {
    // Add trailing 0s to account for the supplied p.
    for (int i = 0; i < p; ++i) {
      n.push_back('0');
    }
    e = 0;
  } else {
    // x != 0
    // Decimal point index.
    int decPt;
    // 1 if negative, 0 else.
    int sign;
    // Points to the end of the string s after it's populated.
    char *sEnd;

    // Store the string representation of n, as provided by dtoa.
    // Use mode=2 and precision=p for the dtoa call (precision dtoa).
    {
      DtoaAllocator<> dalloc{};
      char *s = ::dtoa_fixedpoint(dalloc, x, 2, p, &decPt, &sign, &sEnd);
      n.append(s, sEnd);
      ::g_freedtoa(dalloc, s);
    }

    // Minimum length of the string should be enough to account for
    // p significant digits.
    size_t minNLen = p;
    while (n.size() < minNLen) {
      n.push_back('0');
    }

    // We want the exponent to be one less than the position of the decPt.
    // Example: 123.45 (p=2) has n=12345, decPt=3, so we want e=2.
    e = decPt - 1;

    if (e < -6 || e >= p) {
      // Use scientific notation since the exponent is too big/small.
      if (n.size() > 1) {
        // Add a decimal point if there's enough digits to require it.
        n.insert(n.begin() + 1, '.');
      }

      // Append exponent.
      if (e == 0) {
        n.append("e+0");
      } else {
        llvh::raw_svector_ostream os{n};
        os << llvh::format("e%+d", e);
      }

      // ES5.1 spec says not to return here, and just set m.
      // However, ES6 fixes this bug, which would result in the conditionals
      // below executing even after we generated scientific notation,
      // by telling us to return here instead.

      if (negative) {
        n.insert(n.begin(), '-');
      }
      return runtime.ignoreAllocationFailure(
          StringPrimitive::create(runtime, n));
    }
  }

  // From this point on, we know that -6 <= e < p <= n.size().

  if (e == p - 1) {
    if (negative) {
      n.insert(n.begin(), '-');
    }
    return runtime.ignoreAllocationFailure(StringPrimitive::create(runtime, n));
  }

  // Now we know that -6 <= e < p-1, handle the cases that we need to put a
  // decimal place.
  if (e >= 0) {
    n.insert(n.begin() + (e + 1), '.');
    if (negative) {
      n.insert(n.begin(), '-');
    }
    return runtime.ignoreAllocationFailure(StringPrimitive::create(runtime, n));
  } else {
    // Make a new string m here since it's easier than inserting at the start of
    // n repeatedly.
    llvh::SmallString<32> m{"0."};
    m.reserve(2 + -(e + 1) + n.size());
    for (int i = 0; i < -(e + 1); ++i) {
      m.push_back('0');
    }
    m.append(n);
    if (negative) {
      m.insert(m.begin(), '-');
    }
    return runtime.ignoreAllocationFailure(StringPrimitive::create(runtime, m));
  }
}

} // namespace vm
} // namespace hermes

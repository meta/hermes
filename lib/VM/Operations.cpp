/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/Operations.h"

#include "hermes/FrontEndDefs/Typeof.h"
#include "hermes/Support/Conversions.h"
#include "hermes/Support/FastStrToDouble.h"
#include "hermes/Support/OSCompat.h"
#include "hermes/VM/BigIntPrimitive.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/Casting.h"
#include "hermes/VM/FastArray.h"
#include "hermes/VM/HostModel.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSCallableProxy.h"
#include "hermes/VM/JSError.h"
#include "hermes/VM/JSMapImpl.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/JSRegExp.h"
#include "hermes/VM/PrimitiveBox.h"
#include "hermes/VM/PropertyAccessor.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StaticHUtils.h"
#include "hermes/VM/StringBuilder.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringView.h"

#include "dtoa/dtoa.h"

#include "llvh/ADT/SmallString.h"

#include <cfloat>
#include <cmath>

namespace hermes {
namespace vm {

CallResult<Handle<SymbolID>> valueToSymbolID(
    Runtime &runtime,
    Handle<> nameValHnd) {
  if (nameValHnd->isSymbol()) {
    return Handle<SymbolID>::vmcast(nameValHnd);
  }
  // Convert the value to a string.
  auto res = toString_RJS(runtime, nameValHnd);
  if (res == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;

  // Unique the string.
  return stringToSymbolID(runtime, std::move(*res));
}

HermesValue typeOf(Runtime &runtime, Handle<> valueHandle) {
  switch (valueHandle->getETag()) {
    case HermesValue::ETag::Undefined:
      return HermesValue::encodeStringValue(
          runtime.getPredefinedString(Predefined::undefined));
    case HermesValue::ETag::Null:
      return HermesValue::encodeStringValue(
          runtime.getPredefinedString(Predefined::object));
    case HermesValue::ETag::Str1:
    case HermesValue::ETag::Str2:
      return HermesValue::encodeStringValue(
          runtime.getPredefinedString(Predefined::string));
    case HermesValue::ETag::BigInt1:
    case HermesValue::ETag::BigInt2:
      return HermesValue::encodeStringValue(
          runtime.getPredefinedString(Predefined::bigint));
    case HermesValue::ETag::Bool:
      return HermesValue::encodeStringValue(
          runtime.getPredefinedString(Predefined::boolean));
    case HermesValue::ETag::Symbol:
      return HermesValue::encodeStringValue(
          runtime.getPredefinedString(Predefined::symbol));
    case HermesValue::ETag::Object1:
    case HermesValue::ETag::Object2:
      if (vmisa<Callable>(*valueHandle))
        return HermesValue::encodeStringValue(
            runtime.getPredefinedString(Predefined::function));
      return HermesValue::encodeStringValue(
          runtime.getPredefinedString(Predefined::object));
    default:
      assert(valueHandle->isNumber() && "Invalid type.");
      return HermesValue::encodeStringValue(
          runtime.getPredefinedString(Predefined::number));
  }
}

bool matchTypeOfIs(HermesValue arg, TypeOfIsTypes types) {
  switch (arg.getETag()) {
    case HermesValue::ETag::Undefined:
      return types.hasUndefined();
    case HermesValue::ETag::Null:
      return types.hasNull();
    case HermesValue::ETag::Str1:
    case HermesValue::ETag::Str2:
      return types.hasString();
    case HermesValue::ETag::BigInt1:
    case HermesValue::ETag::BigInt2:
      return types.hasBigint();
    case HermesValue::ETag::Bool:
      return types.hasBoolean();
    case HermesValue::ETag::Symbol:
      return types.hasSymbol();
    case HermesValue::ETag::Object1:
    case HermesValue::ETag::Object2: {
      CellKind kind = static_cast<GCCell *>(arg.getObject())->getKind();
      // Check the CellKind to avoid having to include Callable.h here.
      if (CellKind::CallableKind_first <= kind &&
          kind <= CellKind::CallableKind_last)
        return types.hasFunction();
      else
        return types.hasObject();
    }
    default:
      return types.hasNumber();
  }
}

bool isSameValue(HermesValue x, HermesValue y) {
  // Check for NaN before checking the tag. We have to do this because NaNs may
  // differ in the sign bit, which may result in the tag comparison below
  // incorrectly returning false.
  if (LLVM_UNLIKELY(x.isNaN()) && y.isNaN())
    return true;

  if (x.getTag() != y.getTag()) {
    // If the tags are different, they must be different.
    return false;
  }
  assert(!x.isEmpty() && "Empty cannot be compared");

  // Strings require deep comparison.
  if (x.isString()) {
    // For strings, we compare each character in sequence.
    return x.getString()->equals(y.getString());
  }

  // Bigints also require deep comparison.
  if (x.isBigInt()) {
    // For bigints, perform the numerical comparison.
    return x.getBigInt()->compare(y.getBigInt()) == 0;
  }

  // Otherwise they are identical if the raw bits are the same.
  return x.getRaw() == y.getRaw();
}

CallResult<HermesValue> ordinaryToPrimitive(
    Handle<JSObject> selfHandle,
    Runtime &runtime,
    PreferredType preferredType) {
  GCScope gcScope{runtime};
  assert(
      preferredType != PreferredType::NONE &&
      "OrdinaryToPrimitive requires a type hint");

  for (int i = 0; i < 2; ++i) {
    if (preferredType == PreferredType::STRING) {
      auto propRes = JSObject::getNamed_RJS(
          selfHandle, runtime, Predefined::getSymbolID(Predefined::toString));
      if (propRes == ExecutionStatus::EXCEPTION)
        return ExecutionStatus::EXCEPTION;
      if (auto funcHandle = Handle<Callable>::dyn_vmcast(
              runtime.makeHandle(std::move(*propRes)))) {
        auto callRes =
            funcHandle->executeCall0(funcHandle, runtime, selfHandle);
        if (callRes == ExecutionStatus::EXCEPTION)
          return ExecutionStatus::EXCEPTION;
        if (isPrimitive(callRes->get()))
          return callRes.toCallResultHermesValue();
      }

      // This method failed. Try the other one.
      preferredType = PreferredType::NUMBER;
    } else {
      auto propRes = JSObject::getNamed_RJS(
          selfHandle, runtime, Predefined::getSymbolID(Predefined::valueOf));
      if (propRes == ExecutionStatus::EXCEPTION)
        return ExecutionStatus::EXCEPTION;
      if (auto funcHandle = Handle<Callable>::dyn_vmcast(
              runtime.makeHandle(std::move(*propRes)))) {
        auto callRes =
            funcHandle->executeCall0(funcHandle, runtime, selfHandle);
        if (callRes == ExecutionStatus::EXCEPTION)
          return ExecutionStatus::EXCEPTION;
        if (isPrimitive(callRes->get()))
          return callRes.toCallResultHermesValue();
      }

      // This method failed. Try the other one.
      preferredType = PreferredType::STRING;
    }
  }

  // Nothing succeeded, time to give up.
  return runtime.raiseTypeError("Cannot determine default value of object");
}

/// ES5.1 9.1
CallResult<HermesValue>
toPrimitive_RJS(Runtime &runtime, Handle<> valueHandle, PreferredType hint) {
  assert(!valueHandle->isEmpty() && "empty value is not allowed");

  if (!valueHandle->isObject())
    return *valueHandle;

  // 4. Let exoticToPrim be GetMethod(input, @@toPrimitive).
  auto exoticToPrim = getMethod(
      runtime,
      valueHandle,
      runtime.makeHandle(
          Predefined::getSymbolID(Predefined::SymbolToPrimitive)));
  if (LLVM_UNLIKELY(exoticToPrim == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 6. If exoticToPrim is not undefined, then
  if (vmisa<Callable>(exoticToPrim->getHermesValue())) {
    auto callable = runtime.makeHandle<Callable>(
        dyn_vmcast<Callable>(exoticToPrim->getHermesValue()));
    CallResult<PseudoHandle<>> resultRes = Callable::executeCall1(
        callable,
        runtime,
        valueHandle,
        HermesValue::encodeStringValue(runtime.getPredefinedString(
            hint == PreferredType::NONE         ? Predefined::defaultStr
                : hint == PreferredType::STRING ? Predefined::string
                                                : Predefined::number)));
    if (LLVM_UNLIKELY(resultRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    PseudoHandle<> result = std::move(*resultRes);
    if (!result->isObject()) {
      return result.getHermesValue();
    }
    return runtime.raiseTypeError(
        "Symbol.toPrimitive function must return a primitive");
  }

  // 7. If hint is "default", let hint be "number".
  // 8. Return OrdinaryToPrimitive(input,hint).
  return ordinaryToPrimitive(
      Handle<JSObject>::vmcast(valueHandle),
      runtime,
      hint == PreferredType::NONE ? PreferredType::NUMBER : hint);
}

bool toBoolean(HermesValue value) {
  switch (value.getETag()) {
#ifdef HERMES_SLOW_DEBUG
    case HermesValue::ETag::Invalid:
      llvm_unreachable("invalid value");
#endif // HERMES_SLOW_DEBUG
    case HermesValue::ETag::Empty:
      llvm_unreachable("empty value");
    case HermesValue::ETag::Undefined:
    case HermesValue::ETag::Null:
      return false;
    case HermesValue::ETag::Bool:
      return value.getBool();
    case HermesValue::ETag::Symbol:
    case HermesValue::ETag::Object1:
    case HermesValue::ETag::Object2:
      return true;
    case HermesValue::ETag::BigInt1:
    case HermesValue::ETag::BigInt2:
      return value.getBigInt()->compare(0) != 0;
    case HermesValue::ETag::Str1:
    case HermesValue::ETag::Str2:
      return value.getString()->getStringLength() != 0;
    default: {
      auto m = value.getNumber();
      return !(m == 0 || std::isnan(m));
    }
  }
}

/// ES5.1 9.8.1
CallResult<PseudoHandle<StringPrimitive>> numberToStringPrimitive(
    Runtime &runtime,
    double m) {
  char buf8[hermes::NUMBER_TO_STRING_BUF_SIZE];
  llvh::StringRef str;

  // Optimization: Fast-case for positive integers < 2^31
  if (auto fast = hermes::tryFastPositiveIntToString(m, buf8, sizeof(buf8))) {
    str = *fast;
  } else {
    // These cases are already handled correctly by numberToStringSlowPath().
    // We check them here only so that we can return an interned string instead
    // of allocating a new one, and only after the fast path so that the common
    // integer case does not pay for the comparisons.
    auto getPredefined = [&runtime](Predefined::Str predefinedID) {
      return createPseudoHandle(runtime.getPredefinedString(predefinedID));
    };

    if (std::isnan(m))
      return getPredefined(Predefined::NaN);
    if (m == 0)
      return getPredefined(Predefined::zero);
    if (m == std::numeric_limits<double>::infinity())
      return getPredefined(Predefined::Infinity);
    if (m == -std::numeric_limits<double>::infinity())
      return getPredefined(Predefined::NegativeInfinity);

    // After special cases, run the slow path to convert.
    str = hermes::numberToStringSlowPath(m, buf8, sizeof(buf8));
  }

  auto result =
      StringPrimitive::create(runtime, ASCIIRef(str.data(), str.size()));
  if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return createPseudoHandle(vmcast<StringPrimitive>(*result));
}

CallResult<PseudoHandle<StringPrimitive>> toString_RJS(
    Runtime &runtime,
    Handle<> valueHandle) {
  HermesValue value = valueHandle.get();
  StringPrimitive *result;
  switch (value.getETag()) {
#ifdef HERMES_SLOW_DEBUG
    case HermesValue::ETag::Invalid:
      llvm_unreachable("invalid value");
#endif // HERMES_SLOW_DEBUG
    case HermesValue::ETag::Empty:
      llvm_unreachable("empty value");
    case HermesValue::ETag::BigInt1:
    case HermesValue::ETag::BigInt2: {
      const uint8_t kDefaultRadix = 10;
      auto res = BigIntPrimitive::toString(
          runtime, Handle<BigIntPrimitive>::vmcast(valueHandle), kDefaultRadix);
      if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      result = res->getString();
      break;
    }
    case HermesValue::ETag::Str1:
    case HermesValue::ETag::Str2:
      result = vmcast<StringPrimitive>(value);
      break;
    case HermesValue::ETag::Undefined:
      result = runtime.getPredefinedString(Predefined::undefined);
      break;
    case HermesValue::ETag::Null:
      result = runtime.getPredefinedString(Predefined::null);
      break;
    case HermesValue::ETag::Bool:
      result = value.getBool()
          ? runtime.getPredefinedString(Predefined::trueStr)
          : runtime.getPredefinedString(Predefined::falseStr);
      break;
    case HermesValue::ETag::Object1:
    case HermesValue::ETag::Object2: {
      auto res = toPrimitive_RJS(runtime, valueHandle, PreferredType::STRING);
      if (res == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      return toString_RJS(runtime, runtime.makeHandle(res.getValue()));
    }
    case HermesValue::ETag::Symbol:
      return runtime.raiseTypeError("Cannot convert Symbol to string");
    default:
      return numberToStringPrimitive(runtime, value.getNumber());
  }

  return createPseudoHandle(result);
}

bool isStringWellFormedUnicode(StringPrimitive *string) {
  if (string->isASCII()) {
    // It's impossible for an ASCII string to contain any surrogates, so it must
    // be well formed.
    return true;
  }

  ArrayRef<char16_t> strRef = string->getStringRef<char16_t>();
  // Check for lone surrogates by iterating through the string
  for (size_t i = 0, len = strRef.size(); i < len; ++i) {
    char16_t ch = strRef[i];
    // Check if this character is a surrogate
    if (ch >= UNICODE_SURROGATE_FIRST && ch <= UNICODE_SURROGATE_LAST) {
      // If it's a high surrogate, check if there's a valid low surrogate
      // following
      if (isHighSurrogate(ch)) {
        if (i + 1 < len) {
          char16_t next = strRef[i + 1];
          if (isLowSurrogate(next)) {
            // Valid surrogate pair, skip the low surrogate
            ++i;
            continue;
          }
        }
        // Lone surrogate: high surrogate without low counterpart
        return false;
      } else {
        // Lone surrogate: low surrogate without high counterpart
        return false;
      }
    }
  }

  // No lone surrogates found
  return true;
}

/// ES5.1 9.3.1
static inline double stringToNumber(
    Runtime &runtime,
    Handle<StringPrimitive> strPrim) {
  auto &idTable = runtime.getIdentifierTable();

  // Fast check for special values (no extraneous whitespace).
  if (runtime.symbolEqualsToStringPrim(
          Predefined::getSymbolID(Predefined::Infinity), *strPrim)) {
    return std::numeric_limits<double>::infinity();
  }
  if (runtime.symbolEqualsToStringPrim(
          Predefined::getSymbolID(Predefined::PositiveInfinity), *strPrim)) {
    return std::numeric_limits<double>::infinity();
  }
  if (runtime.symbolEqualsToStringPrim(
          Predefined::getSymbolID(Predefined::NegativeInfinity), *strPrim)) {
    return -std::numeric_limits<double>::infinity();
  }
  if (runtime.symbolEqualsToStringPrim(
          Predefined::getSymbolID(Predefined::NaN), *strPrim)) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Trim string to the interval [begin, end).
  auto orig = StringPrimitive::createStringView(runtime, strPrim);
  auto begin = orig.begin();
  auto end = orig.end();

  // Move begin and end to ignore whitespace.
  while (begin != end &&
         (isWhiteSpaceChar(*begin) || isLineTerminatorChar(*begin))) {
    ++begin;
  }
  while (begin != end &&
         (isWhiteSpaceChar(*(end - 1)) || isLineTerminatorChar(*(end - 1)))) {
    --end;
  }
  // Early return for empty strings (strings only containing whitespace).
  if (begin == end) {
    return 0;
  }

  // Trim the string.
  StringView str16 = orig.slice(begin, end);

  // Slow check for special values.
  // This should only run if user created a string with extra whitespace,
  // since normal uses would get caught by the initial check.
  if (LLVM_UNLIKELY(str16.equals(idTable.getStringView(
          runtime, Predefined::getSymbolID(Predefined::Infinity))))) {
    return std::numeric_limits<double>::infinity();
  }
  if (LLVM_UNLIKELY(str16.equals(idTable.getStringView(
          runtime, Predefined::getSymbolID(Predefined::PositiveInfinity))))) {
    return std::numeric_limits<double>::infinity();
  }
  if (LLVM_UNLIKELY(str16.equals(idTable.getStringView(
          runtime, Predefined::getSymbolID(Predefined::NegativeInfinity))))) {
    return -std::numeric_limits<double>::infinity();
  }
  if (LLVM_UNLIKELY(str16.equals(idTable.getStringView(
          runtime, Predefined::getSymbolID(Predefined::NaN))))) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  auto len = str16.length();

  // Parse hex codes, since dtoa doesn't do it.
  // FIXME: May be inaccurate for some hex values.
  // We need to check other sources first.
  if (len > 2) {
    if (str16[0] == u'0' && letterToLower(str16[1]) == u'x') {
      return parseIntWithRadix(str16.slice(2), 16);
    }
    if (str16[0] == u'0' && letterToLower(str16[1]) == u'o') {
      return parseIntWithRadix(str16.slice(2), 8);
    }
    if (str16[0] == u'0' && letterToLower(str16[1]) == u'b') {
      return parseIntWithRadix(str16.slice(2), 2);
    }
  }

  // Finally, copy 16 bit chars into 8 bit chars and call fastStrToDouble.
  llvh::SmallVector<char, 32> str8(len);
  uint32_t i = 0;
  for (auto c16 : str16) {
    // Check to ensure we only have valid number characters now.
    if ((u'0' <= c16 && c16 <= u'9') || c16 == u'.' ||
        letterToLower(c16) == u'e' || c16 == u'+' || c16 == u'-') {
      str8[i] = static_cast<char>(c16);
    } else {
      return std::numeric_limits<double>::quiet_NaN();
    }
    ++i;
  }

  Char8StrToDoubleParseResult parseRes =
      fastStrToDouble(llvh::ArrayRef<char>{str8});
  if (parseRes.ptr == str8.data() + len) {
    return parseRes.value;
  }

  // If everything failed, return NaN.
  return std::numeric_limits<double>::quiet_NaN();
}

CallResult<HermesValue> toNumber_RJS(Runtime &runtime, Handle<> valueHandle) {
  auto value = valueHandle.get();
  double result;
  switch (value.getETag()) {
#ifdef HERMES_SLOW_DEBUG
    case HermesValue::ETag::Invalid:
      llvm_unreachable("invalid value");
#endif // HERMES_SLOW_DEBUG
    case HermesValue::ETag::Empty:
      llvm_unreachable("empty value");
    case HermesValue::ETag::Object1:
    case HermesValue::ETag::Object2: {
      auto res = toPrimitive_RJS(runtime, valueHandle, PreferredType::NUMBER);
      if (res == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      return toNumber_RJS(runtime, runtime.makeHandle(res.getValue()));
    }
    case HermesValue::ETag::Str1:
    case HermesValue::ETag::Str2:
      result =
          stringToNumber(runtime, Handle<StringPrimitive>::vmcast(valueHandle));
      break;
    case HermesValue::ETag::BigInt1:
    case HermesValue::ETag::BigInt2:
      return runtime.raiseTypeError("Cannot convert BigInt to number");
    case HermesValue::ETag::Undefined:
      result = std::numeric_limits<double>::quiet_NaN();
      break;
    case HermesValue::ETag::Null:
      result = +0.0;
      break;
    case HermesValue::ETag::Bool:
      result = value.getBool();
      break;
    case HermesValue::ETag::Symbol:
      return runtime.raiseTypeError("Cannot convert Symbol to number");
    default:
      // Already have a number, just return it.
      return value;
  }
  return HermesValue::encodeTrustedNumberValue(result);
}

CallResult<HermesValue> toNumeric_RJS(Runtime &runtime, Handle<> valueHandle) {
  GCScopeMarkerRAII marker{runtime};
  auto primValue = toPrimitive_RJS(runtime, valueHandle, PreferredType::NUMBER);

  if (LLVM_UNLIKELY(primValue == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (primValue->isBigInt()) {
    return *primValue;
  }

  return toNumber_RJS(runtime, runtime.makeHandle(*primValue));
}

CallResult<HermesValue> toLength(Runtime &runtime, Handle<> valueHandle) {
  constexpr double highestIntegralDouble =
      ((uint64_t)1 << std::numeric_limits<double>::digits) - 1;
  auto res = toIntegerOrInfinity(runtime, valueHandle);
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto len = res->getNumber();
  if (len <= 0) {
    len = 0;
  } else if (len > highestIntegralDouble) {
    len = highestIntegralDouble;
  }
  return HermesValue::encodeTrustedNumberValue(len);
}

CallResult<HermesValue> toIndex(Runtime &runtime, Handle<> valueHandle) {
  auto value = (valueHandle->isUndefined())
      ? runtime.makeHandle(HermesValue::encodeTrustedNumberValue(0))
      : valueHandle;
  auto res = toIntegerOrInfinity(runtime, value);
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto integerIndex = res->getNumber();
  if (integerIndex < 0) {
    return runtime.raiseRangeError("A negative value cannot be an index");
  }
  auto integerIndexHandle =
      runtime.makeHandle(HermesValue::encodeTrustedNumberValue(integerIndex));
  res = toLength(runtime, integerIndexHandle);
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto index = res.getValue();
  if (index.getNumber() != integerIndex) {
    return runtime.raiseRangeError(
        "The value given for the index must be between 0 and 2 ^ 53 - 1");
  }
  return res;
}

CallResult<HermesValue> toIntegerOrInfinity(
    Runtime &runtime,
    Handle<> valueHandle) {
  auto res = toNumber_RJS(runtime, valueHandle);
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  double num = res->getNumber();

  double result;
  if (std::isnan(num)) {
    result = 0;
  } else {
    result = std::trunc(num);
  }

  return HermesValue::encodeTrustedNumberValue(result);
}

/// Conversion of HermesValues to integers.
template <typename T>
static inline CallResult<HermesValue> toInt(
    Runtime &runtime,
    Handle<> valueHandle) {
  auto res = toNumber_RJS(runtime, valueHandle);
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  double num = res->getNumber();
  T result = static_cast<T>(hermes::truncateToInt32(num));
  return HermesValue::encodeTrustedNumberValue(result);
}

CallResult<HermesValue> toInt8(Runtime &runtime, Handle<> valueHandle) {
  return toInt<int8_t>(runtime, valueHandle);
}

CallResult<HermesValue> toInt16(Runtime &runtime, Handle<> valueHandle) {
  return toInt<int16_t>(runtime, valueHandle);
}

CallResult<HermesValue> toInt32_RJS(Runtime &runtime, Handle<> valueHandle) {
  return toInt<int32_t>(runtime, valueHandle);
}

CallResult<HermesValue> toUInt8(Runtime &runtime, Handle<> valueHandle) {
  return toInt<uint8_t>(runtime, valueHandle);
}

uint8_t toUInt8Clamp(double number) {
  // 3. If number is NaN, return +0.
  // 4. If number <= 0, return +0.
  // Not < so that NaN coerces to 0.
  // NOTE: this check correctly rounds numbers less than 0.5
  if (!(number >= 0.5)) {
    return 0;
  }

  // 5. If number >= 255, return 255.
  if (number > 255) {
    return 255;
  }

  // The next steps are the equivalent of the spec's round-to-even requirement.
  // Round up and then do the even/odd check.
  double toTruncate = number + 0.5;
  uint8_t x = static_cast<uint8_t>(toTruncate);

  // If it was a tie (i.e. it ended in 0.5) then
  if (x == toTruncate) {
    // number ended in 0.5 and was rounded up, reduce by 1 if odd,
    // else leave the same.
    // That is the same as unsetting the least significant bit.
    return (x & ~1);
  } else {
    // number did not end in 0.5, don't need to check the parity.
    return x;
  }
}

CallResult<HermesValue> toUInt8Clamp(Runtime &runtime, Handle<> valueHandle) {
  // 1. Let number be toNumber_RJS(argument)
  auto res = toNumber_RJS(runtime, valueHandle);
  if (res == ExecutionStatus::EXCEPTION) {
    // 2. ReturnIfAbrupt(number)
    return ExecutionStatus::EXCEPTION;
  }
  return HermesValue::encodeTrustedNumberValue(toUInt8Clamp(res->getNumber()));
}

CallResult<HermesValue> toUInt16(Runtime &runtime, Handle<> valueHandle) {
  return toInt<uint16_t>(runtime, valueHandle);
}

CallResult<HermesValue> toUInt32_RJS(Runtime &runtime, Handle<> valueHandle) {
  return toInt<uint32_t>(runtime, valueHandle);
}

CallResult<Handle<JSObject>> getPrimitivePrototype(
    Runtime &runtime,
    Handle<> base) {
  switch (base->getETag()) {
#ifdef HERMES_SLOW_DEBUG
    case HermesValue::ETag::Invalid:
      llvm_unreachable("invalid value");
#endif // HERMES_SLOW_DEBUG
    case HermesValue::ETag::Empty:
      llvm_unreachable("empty value");
    case HermesValue::ETag::Object1:
    case HermesValue::ETag::Object2:
      llvm_unreachable("object value");
    case HermesValue::ETag::Undefined:
      return runtime.raiseTypeError("Cannot convert undefined value to object");
    case HermesValue::ETag::Null:
      return runtime.raiseTypeError("Cannot convert null value to object");
    case HermesValue::ETag::Str1:
    case HermesValue::ETag::Str2:
      return Handle<JSObject>::vmcast(&runtime.stringPrototype);
    case HermesValue::ETag::BigInt1:
    case HermesValue::ETag::BigInt2:
      return Handle<JSObject>::vmcast(&runtime.bigintPrototype);
    case HermesValue::ETag::Bool:
      return Handle<JSObject>::vmcast(&runtime.booleanPrototype);
    case HermesValue::ETag::Symbol:
      return Handle<JSObject>::vmcast(&runtime.symbolPrototype);
    default:
      assert(base->isNumber() && "Unknown tag in getPrimitivePrototype.");
      return Handle<JSObject>::vmcast(&runtime.numberPrototype);
  }
}

CallResult<HermesValue> toObject(Runtime &runtime, Handle<> valueHandle) {
  auto value = valueHandle.get();
  switch (value.getETag()) {
#ifdef HERMES_SLOW_DEBUG
    case HermesValue::ETag::Invalid:
      llvm_unreachable("invalid value");
#endif // HERMES_SLOW_DEBUG
    case HermesValue::ETag::Empty:
      llvm_unreachable("empty value");
    case HermesValue::ETag::Undefined:
      return runtime.raiseTypeError("Cannot convert undefined value to object");
    case HermesValue::ETag::Null:
      return runtime.raiseTypeError("Cannot convert null value to object");
    case HermesValue::ETag::Object1:
    case HermesValue::ETag::Object2:
      return value;
    case HermesValue::ETag::Bool:
      return JSBoolean::create(
                 runtime,
                 value.getBool(),
                 Handle<JSObject>::vmcast(&runtime.booleanPrototype))
          .getHermesValue();
    case HermesValue::ETag::BigInt1:
    case HermesValue::ETag::BigInt2:
      return JSBigInt::create(
                 runtime,
                 Handle<BigIntPrimitive>::vmcast(valueHandle),
                 Handle<JSObject>::vmcast(&runtime.bigintPrototype))
          .getHermesValue();
    case HermesValue::ETag::Str1:
    case HermesValue::ETag::Str2: {
      auto res = JSString::create(
          runtime,
          Handle<StringPrimitive>::vmcast(valueHandle),
          Handle<JSObject>::vmcast(&runtime.stringPrototype));
      if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      return res->getHermesValue();
    }
    case HermesValue::ETag::Symbol:
      return JSSymbol::create(
                 runtime,
                 *Handle<SymbolID>::vmcast(valueHandle),
                 Handle<JSObject>::vmcast(&runtime.symbolPrototype))
          .getHermesValue();
    default:
      assert(valueHandle->isNumber() && "Unknown tag in toObject.");
      return JSNumber::create(
                 runtime, value.getNumber(), runtime.numberPrototype)
          .getHermesValue();
  }
}

ExecutionStatus amendPropAccessErrorMsgWithPropName(
    Runtime &runtime,
    Handle<> valueHandle,
    llvh::StringRef operationStr,
    SymbolID id) {
  if (!valueHandle->isNull() && !valueHandle->isUndefined()) {
    // If value is not null/undefined, fall back to the original exception.
    return ExecutionStatus::EXCEPTION;
  }
  assert(!runtime.getThrownValue().isEmpty() && "Error must have been thrown");
  // Clear the error first because we will re-throw.
  runtime.clearThrownValue();

  // Construct an error message that contains the property name.
  llvh::StringRef valueStr = valueHandle->isNull() ? "null" : "undefined";
  return runtime.raiseTypeError(
      TwineChar16("Cannot ") + operationStr + " property '" +
      runtime.getIdentifierTable().getStringView(runtime, id) + "' of " +
      valueStr);
}

/// Implement a BigInt vs. String comparison operation using a user-provided
/// \p comparator. Note that \p leftHandle is a Handle<BigIntPrimitive> to
/// ensure the caller is putting the BigInt in the lhs (and adjusting \p
/// comparator appropriately).
/// \return false if StringToBigInt( \p rightHandle ) is undefined, otherwise
/// returns \p comparator ( \p leftHandle <=> \p righHandle ).
static CallResult<bool> compareBigIntAndString(
    Runtime &runtime,
    Handle<BigIntPrimitive> leftHandle,
    Handle<> rightHandle,
    bool (*comparator)(int)) {
  assert(rightHandle->isString() && "rhs should be string");

  auto bigintRight = stringToBigInt(runtime, rightHandle);
  if (LLVM_UNLIKELY(bigintRight == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (bigintRight->isUndefined()) { // Non-compliance: should be undefined.
    return false;
  }
  assert(bigintRight->isBigInt() && "stringToBigInt resulted in non-bigint");
  return comparator(leftHandle->compare(bigintRight->getBigInt()));
}

/// Implement a BigInt vs. Number comparison operation using a user-provided
/// \p comparator. Note that \p leftHandle is a Handle<BigIntPrimitive> to
/// ensure the caller is putting the BigInt in the lhs (and adjusting \p
/// comparator appropriately).
/// \return false if \p right is NaN, otherwise returns
/// \p comparator ( \p leftHandle <=> \p righHandle ).
static CallResult<bool> compareBigIntAndNumber(
    Runtime &runtime,
    Handle<BigIntPrimitive> leftHandle,
    double right,
    bool (*comparator)(int)) {
  switch (std::fpclassify(right)) {
    case FP_NAN:
      // BigInt comparison to NaN is always false.
      return false;
    case FP_INFINITE:
      // If rhs is +infinite, it is greater than lhs; otherwise, it is less than
      // rhs.
      return comparator(right > 0 ? -1 : 1);
    default:
      break;
  }

  // Split the rhs into integral and fractional parts.
  double integralPart;
  const double fractionalPart = std::modf(right, &integralPart);

  // Now use the rhs' integral part to create a new BigInt, which is compared to
  // lhs.
  auto rightHandle = BigIntPrimitive::fromDouble(runtime, integralPart);
  if (LLVM_UNLIKELY(rightHandle == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // If rhs' integral part is different than lhs, then use the integral parts'
  // comparison to decide the result.
  if (int comparisonResult = leftHandle->compare(rightHandle->getBigInt())) {
    return comparator(comparisonResult);
  }

  // Lhs' and rhs' integral parts are equal, thus resort the rhs' fractional
  // part.
  if (fractionalPart != 0) {
    //  If rhs is negative, then it is smaller than lhs; otherwise, it is
    //  greater.
    return comparator(right < 0 ? 1 : -1);
  }

  // Lhs' and rhs' integral parts are equal, and rhs does not have a fractional
  // part (it is zero), thus they are equal.
  return comparator(0);
}

/// Implement a comparison operator. First both operands a converted to
/// primitives. If they both end up being strings, a lexicographical comparison
/// is performed. Otherwise both operands are converted to numbers and the
/// values are compared.
/// \param oper is the comparison operator to use when comparing numbers.
#define IMPLEMENT_COMPARISON_OP(name, oper)                           \
  CallResult<bool> name(                                              \
      Runtime &runtime, Handle<> leftHandle, Handle<> rightHandle) {  \
    auto resLeft =                                                    \
        toPrimitive_RJS(runtime, leftHandle, PreferredType::NUMBER);  \
    if (resLeft == ExecutionStatus::EXCEPTION)                        \
      return ExecutionStatus::EXCEPTION;                              \
    MutableHandle<> left(runtime, resLeft.getValue());                \
                                                                      \
    auto resRight =                                                   \
        toPrimitive_RJS(runtime, rightHandle, PreferredType::NUMBER); \
    if (resRight == ExecutionStatus::EXCEPTION)                       \
      return ExecutionStatus::EXCEPTION;                              \
    MutableHandle<> right(runtime, resRight.getValue());              \
                                                                      \
    /* If both are strings, we must do a string comparison.*/         \
    if (left->isString() && right->isString()) {                      \
      return left->getString()->compare(right->getString()) oper 0;   \
    }                                                                 \
                                                                      \
    if (left->isBigInt() && right->isString()) {                      \
      return compareBigIntAndString(                                  \
          runtime,                                                    \
          Handle<BigIntPrimitive>::vmcast(left),                      \
          right,                                                      \
          [](int result) { return result oper 0; });                  \
    }                                                                 \
                                                                      \
    if (left->isString() && right->isBigInt()) {                      \
      return compareBigIntAndString(                                  \
          runtime,                                                    \
          Handle<BigIntPrimitive>::vmcast(right),                     \
          left,                                                       \
          [](int result) { return -result oper 0; });                 \
    }                                                                 \
                                                                      \
    /* Convert both to a number and compare the numbers. */           \
    resLeft = toNumeric_RJS(runtime, left);                           \
    if (resLeft == ExecutionStatus::EXCEPTION)                        \
      return ExecutionStatus::EXCEPTION;                              \
    left = resLeft.getValue();                                        \
    resRight = toNumeric_RJS(runtime, right);                         \
    if (resRight == ExecutionStatus::EXCEPTION)                       \
      return ExecutionStatus::EXCEPTION;                              \
    right = resRight.getValue();                                      \
                                                                      \
    if (left->isNumber() && right->isNumber()) {                      \
      return left->getNumber() oper right->getNumber();               \
    } else if (left->isBigInt() && right->isBigInt()) {               \
      return left->getBigInt()->compare(right->getBigInt()) oper 0;   \
    }                                                                 \
                                                                      \
    if (left->isBigInt() && right->isNumber()) {                      \
      return compareBigIntAndNumber(                                  \
          runtime,                                                    \
          Handle<BigIntPrimitive>::vmcast(left),                      \
          right->getNumber(),                                         \
          [](int result) { return result oper 0; });                  \
    }                                                                 \
    assert(                                                           \
        left->isNumber() && right->isBigInt() &&                      \
        "expecting one number and one bigint");                       \
    return compareBigIntAndNumber(                                    \
        runtime,                                                      \
        Handle<BigIntPrimitive>::vmcast(right),                       \
        left->getNumber(),                                            \
        [](int result) { return -result oper 0; });                   \
  }

IMPLEMENT_COMPARISON_OP(lessOp_RJS, <);
IMPLEMENT_COMPARISON_OP(greaterOp_RJS, >);
IMPLEMENT_COMPARISON_OP(lessEqualOp_RJS, <=);
IMPLEMENT_COMPARISON_OP(greaterEqualOp_RJS, >=);

namespace {

enum class EqualityResult { Unknown = -1, NotEqual, Equal };

/// Perform a fast equality check to see if a given double and a given
/// BigInt are equal. If both numbers are small enough for a fast
/// comparison, this will return `Equal` or `NotEqual`. Otherwise, if
/// the numbers are not small enough to be quickly compared, this will
/// return `Unknown`, which denotes that a non-fast-path check is needed.
EqualityResult areEqualSmallNumbers(const double x, BigIntPrimitive *y) {
  // Doubles have a 53-bit mantissa, so only values from 0 to 2^53 - 1 are
  // contiguously representable.
  constexpr int64_t MaxSafeIntegerValueOfDouble =
      (int64_t{1} << std::numeric_limits<double>::digits) - 1;

  // Do a quick pass for single digit values. This prevents us from
  // having to instantiate a BigInt object for small numbers. We can
  // only do this if we can cast our single BigInt digit to a double
  // without loss of precision.
  if (y->isTruncationToSingleDigitLossless(true)) {
    const auto yDigit =
        static_cast<bigint::SignedBigIntDigitType>(y->truncateToSingleDigit());

    if (std::abs(yDigit) < MaxSafeIntegerValueOfDouble) {
      // `false` and `true` are safely castable directly to EqualityResults
      return static_cast<EqualityResult>(static_cast<double>(yDigit) == x);
    }
  }

  // y (the BigInt) is greater than MaxSafeIntegerValueOfDouble. Thus, if
  // x (the double) is less than MaxSafeIntegerValueOfDouble they are
  // certainly NotEqual.
  if (std::abs(x) < static_cast<double>(MaxSafeIntegerValueOfDouble)) {
    static_assert(
        std::numeric_limits<bigint::SignedBigIntDigitType>::max() >
        MaxSafeIntegerValueOfDouble);
    return EqualityResult::NotEqual;
  }

  return EqualityResult::Unknown;
}
} // namespace

/// ES11 7.2.15 Abstract Equality Comparison
CallResult<bool>
abstractEqualityTest_RJS(Runtime &runtime, Handle<> xHandle, Handle<> yHandle) {
  // Fast path: handle common same-type comparisons without allocating
  // MutableHandles, which avoids GCScope overhead for the frequent cases.
  {
    HermesValue xFast = xHandle.get();
    HermesValue yFast = yHandle.get();
    assert(
        !xFast.isEmpty() && !yFast.isEmpty() && "invalid value for comparison");

    // Both numbers: compare as doubles (handles NaN correctly).
    if (LLVM_LIKELY(xFast.isNumber())) {
      if (LLVM_LIKELY(yFast.isNumber()))
        return xFast.getNumber() == yFast.getNumber();
    } else if (xFast.getRaw() == yFast.getRaw()) {
      // Identical non-number values are always equal: covers undefined===
      // undefined, null===null, same bool, same object, same string pointer.
      return true;
    } else if (xFast.isNull() || xFast.isUndefined()) {
      // null == undefined and undefined == null per ES11 7.2.15 steps 2-3.
      // Also handles null != <non-null-non-undefined> and undefined !=
      // <non-null-non-undefined>.
      return yFast.isNull() || yFast.isUndefined();
    } else if (yFast.isNull() || yFast.isUndefined()) {
      // <non-null-non-undefined> != null and != undefined.
      return false;
    }
  }

  MutableHandle<> x{runtime, xHandle.get()};
  MutableHandle<> y{runtime, yHandle.get()};

  while (true) {
    // Combine tags for use in the switch statement. Use NativeValueTag as a
    // placeholder for numbers.
    assert(!x->isEmpty() && !y->isEmpty() && "invalid value for comparison");

    // The following macros are used to generate the switch cases using
    // HermesValue::combineETags; an S in the name means it is a single ETag
    // (e.g., ETag::Bool), while M means it is a multi ETag (e.g., ETag::Object1
    // and ETag::Object2).
#define CASE_S_S(typeA, typeB)    \
  case HermesValue::combineETags( \
      HermesValue::ETag::typeA, HermesValue::ETag::typeB):

#define CASE_S_M(typeA, typeB) \
  CASE_S_S(typeA, typeB##1)    \
  CASE_S_S(typeA, typeB##2)

#define CASE_M_S(typeA, typeB) \
  CASE_S_S(typeA##1, typeB)    \
  CASE_S_S(typeA##2, typeB)

#define CASE_M_M(typeA, typeB) \
  CASE_M_S(typeA, typeB##1)    \
  CASE_M_S(typeA, typeB##2)

// NUMBER_TAG is a "virtual" ETag member that is used to tag numbers (which
// don't have a tag assigned to them). It reuses ETag::Empty there will
// never be any empty values in this part of the code.
#define NUMBER_TAG Empty

    // Tag numbers as with the "virtual" ETag member NUMBER_TAG, and use default
    // tag values for everything else.
    HermesValue::ETag xType =
        x->isNumber() ? HermesValue::ETag::NUMBER_TAG : x->getETag();
    HermesValue::ETag yType =
        y->isNumber() ? HermesValue::ETag::NUMBER_TAG : y->getETag();

    switch (HermesValue::combineETags(xType, yType)) {
      // 1. If Type(x) is the same as Type(y), then
      // a. Return the result of performing Strict Equality Comparison x === y.
      CASE_S_S(Undefined, Undefined)
      CASE_S_S(Null, Null) {
        return true;
      }
      CASE_S_S(NUMBER_TAG, NUMBER_TAG) {
        return x->getNumber() == y->getNumber();
      }
      CASE_M_M(Str, Str) {
        return x->getString()->equals(y->getString());
      }
      CASE_M_M(BigInt, BigInt) {
        return x->getBigInt()->compare(y->getBigInt()) == 0;
      }
      CASE_S_S(Bool, Bool)
      CASE_S_S(Symbol, Symbol)
      CASE_M_M(Object, Object) {
        return x->getRaw() == y->getRaw();
      }
      // 2. If x is null and y is undefined, return true.
      // 3. If x is undefined and y is null, return true.
      CASE_S_S(Undefined, Null)
      CASE_S_S(Null, Undefined) {
        return true;
      }
      // 4. If Type(x) is Number and Type(y) is String, return the result of the
      // comparison x == ! ToNumber(y).
      CASE_S_M(NUMBER_TAG, Str) {
        return x->getNumber() ==
            stringToNumber(runtime, Handle<StringPrimitive>::vmcast(y));
      }
      // 5. If Type(x) is String and Type(y) is Number, return the result of the
      // comparison ! ToNumber(x) == y.
      CASE_M_S(Str, NUMBER_TAG) {
        return stringToNumber(runtime, Handle<StringPrimitive>::vmcast(x)) ==
            y->getNumber();
      }
      // 6. If Type(x) is BigInt and Type(y) is String, then
      CASE_M_M(BigInt, Str) {
        // a. Let n be ! StringToBigInt(y).
        auto n = stringToBigInt(runtime, y);
        if (LLVM_UNLIKELY(n == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        // b. If n is NaN, return false.
        // N.B.: this has been amended in ES2023 to read
        //       If n is undefined, return false.
        if (n->isUndefined()) {
          return false;
        }
        // c. Return the result of the comparison x == n.
        y = n.getValue();
        break;
      }
      // 7. If Type(x) is String and Type(y) is BigInt, return the result of the
      // comparison y == x.
      CASE_M_M(Str, BigInt) {
        std::swap(x, y);
        break;
      }
      // 8. If Type(x) is Boolean, return the result of the comparison !
      // ToNumber(x) == y.
      CASE_S_S(Bool, NUMBER_TAG) {
        // Do both conversions and check numerical equality.
        return static_cast<double>(x->getBool()) == y->getNumber();
      }
      CASE_S_M(Bool, Str) {
        // Do string parsing and check double equality.
        return static_cast<double>(x->getBool()) ==
            stringToNumber(runtime, Handle<StringPrimitive>::vmcast(y));
      }
      CASE_S_M(Bool, BigInt) {
        return y->getBigInt()->compare(static_cast<int32_t>(x->getBool())) == 0;
      }
      CASE_S_M(Bool, Object) {
        x = HermesValue::encodeTrustedNumberValue(x->getBool());
        break;
      }
      // 9. If Type(y) is Boolean, return the result of the comparison x == !
      // ToNumber(y).
      CASE_S_S(NUMBER_TAG, Bool) {
        return x->getNumber() == static_cast<double>(y->getBool());
      }
      CASE_M_S(Str, Bool) {
        return stringToNumber(runtime, Handle<StringPrimitive>::vmcast(x)) ==
            static_cast<double>(y->getBool());
      }
      CASE_M_S(BigInt, Bool) {
        return x->getBigInt()->compare(static_cast<int32_t>(y->getBool())) == 0;
      }
      CASE_M_S(Object, Bool) {
        y = HermesValue::encodeTrustedNumberValue(y->getBool());
        break;
      }
      // 10. If Type(x) is either String, Number, BigInt, or Symbol and Type(y)
      // is Object, return the result of the comparison x == ToPrimitive(y).
      CASE_M_M(Str, Object)
      CASE_M_M(BigInt, Object)
      CASE_S_M(Symbol, Object)
      CASE_S_M(NUMBER_TAG, Object) {
        auto status = toPrimitive_RJS(runtime, y, PreferredType::NONE);
        if (status == ExecutionStatus::EXCEPTION) {
          return ExecutionStatus::EXCEPTION;
        }
        y = status.getValue();
        break;
      }
      // 11. If Type(x) is Object and Type(y) is either String, Number, BigInt,
      // or Symbol, return the result of the comparison ToPrimitive(x) == y.
      CASE_M_M(Object, Str)
      CASE_M_M(Object, BigInt)
      CASE_M_S(Object, Symbol)
      CASE_M_S(Object, NUMBER_TAG) {
        auto status = toPrimitive_RJS(runtime, x, PreferredType::NONE);
        if (status == ExecutionStatus::EXCEPTION) {
          return ExecutionStatus::EXCEPTION;
        }
        x = status.getValue();
        break;
      }
      // 12. If Type(x) is BigInt and Type(y) is Number, or if Type(x) is
      // Number and Type(y) is BigInt, then a. If x or y are any of NaN, +∞,
      // or -∞, return false. b. If the mathematical value of x is equal to
      // the mathematical value of y, return true; otherwise return false.
      CASE_M_S(BigInt, NUMBER_TAG) {
        std::swap(x, y);
        [[fallthrough]];
      }
      CASE_S_M(NUMBER_TAG, BigInt) {
        if (!isIntegralNumber(x->getNumber())) {
          return false;
        }

        EqualityResult areEqual =
            areEqualSmallNumbers(x->getNumber(), y->getBigInt());
        if (areEqual != EqualityResult::Unknown) {
          return areEqual == EqualityResult::Equal ? true : false;
        }

        auto xAsBigInt = BigIntPrimitive::fromDouble(runtime, x->getNumber());
        if (LLVM_UNLIKELY(xAsBigInt == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        return xAsBigInt->getBigInt()->compare(y->getBigInt()) == 0;
      }

      // 13. Return false.
      default:
        return false;
    }

#undef CASE_S_S
#undef CASE_S_M
#undef CASE_M_S
#undef CASE_M_M
#undef NUMBER_TAG
  }
}

bool strictEqualityTest(HermesValue x, HermesValue y) {
  // Numbers are special because they can have different tags and they don't
  // obey bit-exact equality (because of NaN).
  if (x.isNumber())
    return y.isNumber() && x.getNumber() == y.getNumber();
  // If they are not numbers and are bit exact, they must be the same.
  if (x.getRaw() == y.getRaw())
    return true;
  // All the rest of the cases need to have the same tags.
  if (x.getTag() != y.getTag())
    return false;
  // Strings need deep comparison.
  if (x.isString())
    return x.getString()->equals(y.getString());

  // The only remaining case is bigint, which also needs a deep comparison.
  return x.isBigInt() && x.getBigInt()->compare(y.getBigInt()) == 0;
}

CallResult<HermesValue>
addOp_RJS(Runtime &runtime, Handle<> xHandle, Handle<> yHandle) {
  // Fast path: both operands are already strings, concat directly.
  // Skips toPrimitive + toString (both no-ops for strings) and 4 handle allocs.
  if (xHandle->isString() && yHandle->isString()) {
    return StringPrimitive::concat(
        runtime,
        Handle<StringPrimitive>::vmcast(xHandle),
        Handle<StringPrimitive>::vmcast(yHandle));
  }
  auto resX = toPrimitive_RJS(runtime, xHandle, PreferredType::NONE);
  if (resX == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto xPrim = runtime.makeHandle(resX.getValue());

  auto resY = toPrimitive_RJS(runtime, yHandle, PreferredType::NONE);
  if (resY == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto yPrim = runtime.makeHandle(resY.getValue());

  // If one of the values is a string, concatenate as strings.
  if (xPrim->isString() || yPrim->isString()) {
    auto resX = toString_RJS(runtime, xPrim);
    if (resX == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    auto xStr = runtime.makeHandle(std::move(*resX));

    auto resY = toString_RJS(runtime, yPrim);
    if (resY == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    auto yStr = runtime.makeHandle(std::move(*resY));

    return StringPrimitive::concat(runtime, xStr, yStr);
  }

  // xPrim and yPrim are primitives; hence, they are already bigints, or they
  // will never be bigints.
  if (LLVM_LIKELY(!xPrim->isBigInt())) {
    // xPrim is not a bigint; thus this is Number + Number.
    auto res = toNumber_RJS(runtime, xPrim);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    const double xNum = res->getNumber();
    // N.B.: toNumber(yPrim) will raise an TypeError if yPrim is bigint, which
    // is the correct exception to be raised when trying to perform
    // Number + BigInt. This avoids the need to check if yPrim is a bigint.
    res = toNumber_RJS(runtime, yPrim);
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    const double yNum = res->getNumber();
    return HermesValue::encodeTrustedNumberValue(xNum + yNum);
  }

  // yPrim is a primitive; therefore it is already a BigInt, or it will never be
  // one.
  if (!yPrim->isBigInt()) {
    return runtime.raiseTypeErrorForValue(
        "Cannot convert ", yHandle, " to BigInt");
  }

  return BigIntPrimitive::add(
      runtime,
      runtime.makeHandle(xPrim->getBigInt()),
      runtime.makeHandle(yPrim->getBigInt()));
}

static const size_t MIN_RADIX = 2;
static const size_t MAX_RADIX = 36;

static inline char toRadixChar(unsigned x, unsigned radix) {
  const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  static_assert(sizeof(chars) - 1 == MAX_RADIX, "Invalid chars array");
  assert(
      x < radix && x < std::strlen(chars) &&
      "invalid number to radix conversion");
  return chars[x];
}

/// \return the exponent component of the double \p x.
static inline int doubleExponent(double x) {
  int e;
  std::frexp(x, &e);
  return e;
}

Handle<StringPrimitive>
numberToStringWithRadix(Runtime &runtime, double number, unsigned radix) {
  (void)MIN_RADIX;
  (void)MAX_RADIX;
  assert(MIN_RADIX <= radix && radix <= MAX_RADIX && "Invalid radix");
  // Two parts of the final result: integer part and fractional part.
  llvh::SmallString<64> result{};

  // Used to store just the fractional part of the string (not including '.').
  llvh::SmallString<32> fStr{};

  // If negative, treat as if positive and add a '-' later.
  bool negative = false;
  if (number < 0) {
    negative = true;
    number = -number;
  }

  // Split number into integer and fractional parts.
  double iPart;
  double fPart = std::modf(number, &iPart);

  // If there's a fractional part, convert it and store in fStr.
  if (fPart != 0) {
    // Distance to the next double value.
    double next =
        std::nextafter(number, std::numeric_limits<double>::infinity());
    double minDenorm =
        std::nextafter(0.0, std::numeric_limits<double>::infinity());

    // Precision of the input (half the distance to the next double).
    // We only compute digits up to that precision.
    // Ensure that delta > 0 by clamping it by the min denormalized positive
    // double number.
    double delta = std::max(0.5 * (next - number), minDenorm);

    while (fPart > delta) {
      // Multiply by radix to find the next digit.
      fPart *= radix;
      delta *= radix;
      // Write the next digit.
      unsigned digit = static_cast<unsigned>(fPart);
      fStr.push_back(toRadixChar(digit, radix));
      // Remove current digit from fPart to prepare for next iteration.
      fPart -= digit;
      // Round-to-even.
      if (fPart > 0.5 || (fPart == 0.5 && (digit & 1))) {
        // Must round up, necessitating changing written digits.
        if (fPart + delta > 1) {
          // Round because printing the next closest double would not give
          // closer results than rounding. The distance between the next
          // double and this one is large enough that at this point, we're
          // doing worse than rounding up if we were to print out the next
          // double precisely.
          while (true) {
            // Rounding requires backtracking to fix everything up.
            if (fStr.size() == 0) {
              // Rounding failed to stop in the fractional part,
              // so carry over to the integral part.
              ++iPart;
              break;
            }
            // Iterator to the last digit of the string.
            char &c = fStr.back();
            unsigned digitForC = c <= '9' ? c - '0' : c - 'a' + 10;
            if (digitForC + 1 < radix) {
              // Can increment this digit, and we're done.
              c = toRadixChar(digitForC + 1, radix);
              break;
            }
            // We weren't able to increment, so this will be a trailing 0,
            // which we don't want to keep around anyway. So, pop the last
            // digit off the string and continue on.
            fStr.pop_back();
          }
          // Rounded off the number, done writing the fractional string.
          break;
        }
      }
    }
  }

  // Now, create the integer part.
  if (iPart == 0) {
    result.push_back('0');
  } else {
    // Write the number backwards, then reverse it. This simplifies the code.

    // Physical mantissa size.
    // Hidden bit is not included because it's not after the decimal point in
    // the binary scientific notation representation of a double.
    // We use this to calculate whether we have the precision required to know
    // what the next digit is going to be.
    constexpr const int MANTISSA_SIZE = DBL_MANT_DIG - 1;

    // Handle trailing zeros.
    while (doubleExponent(iPart / radix) > MANTISSA_SIZE) {
      // (iPart / radix) doesn't have enough precision to be useful here,
      // because its exponent is larger than the number of bits that the
      // mantissa can encode. So, just put a trailing zero, divide by radix,
      // and move on to the next digit.
      result.push_back('0');
      iPart /= radix;
    }

    // Print the rest of the string when we know we have enough precision to
    // do so.
    while (iPart > 0) {
      // Cast digit to int because we know 2 <= digit <= 36.
      int digit = static_cast<int>(std::fmod(iPart, radix));
      result.push_back(toRadixChar(digit, radix));
      iPart = (iPart - digit) / radix;
    }

    // Int string was generated in reverse, so flip it.
    std::reverse(result.begin(), result.end());
  }

  // Concatenate the fractional string on if it exists.
  if (!fStr.empty()) {
    result += '.';
    result += fStr;
  }

  // Account for negative numbers.
  if (negative) {
    result.insert(result.begin(), '-');
  }

  return runtime.makeHandle<StringPrimitive>(runtime.ignoreAllocationFailure(
      StringPrimitive::create(runtime, result)));
}

CallResult<PseudoHandle<>>
getMethod(Runtime &runtime, Handle<> O, Handle<> key) {
  GCScopeMarkerRAII gcScope{runtime};
  auto objRes = toObject(runtime, O);
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto obj = runtime.makeHandle<JSObject>(*objRes);
  auto funcRes = JSObject::getComputed_RJS(obj, runtime, key);
  if (LLVM_UNLIKELY(funcRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if ((*funcRes)->isUndefined() || (*funcRes)->isNull()) {
    return PseudoHandle<>::create(HermesValue::encodeUndefinedValue());
  }
  if (!vmisa<Callable>(funcRes->get())) {
    return runtime.raiseTypeError("Could not get callable method from object");
  }
  return funcRes;
}

CallResult<CheckedIteratorRecord> getCheckedIterator(
    Runtime &runtime,
    Handle<> obj,
    llvh::Optional<Handle<Callable>> methodOpt) {
  auto uncheckedIterRecordRes = getIterator(runtime, obj, methodOpt);
  if (uncheckedIterRecordRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto uncheckedIterRecord = uncheckedIterRecordRes.getValue();
  if (LLVM_UNLIKELY(!vmisa<Callable>(uncheckedIterRecord.nextMethod.get()))) {
    return runtime.raiseTypeError("'next' method on iterator must be callable");
  }
  return CheckedIteratorRecord{
      std::move(uncheckedIterRecord.iterator),
      Handle<Callable>::vmcast(std::move(uncheckedIterRecord.nextMethod))};
}

CallResult<UncheckedIteratorRecord> getIterator(
    Runtime &runtime,
    Handle<> obj,
    llvh::Optional<Handle<Callable>> methodOpt) {
  MutableHandle<Callable> method{runtime};
  if (LLVM_LIKELY(!methodOpt.hasValue())) {
    auto methodRes = getMethod(
        runtime,
        obj,
        runtime.makeHandle(
            Predefined::getSymbolID(Predefined::SymbolIterator)));
    if (LLVM_UNLIKELY(methodRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (!vmisa<Callable>(methodRes->getHermesValue())) {
      return runtime.raiseTypeError("iterator method is not callable");
    }
    method = vmcast<Callable>(methodRes->getHermesValue());
  } else {
    method = **methodOpt;
  }
  auto iteratorRes = Callable::executeCall0(method, runtime, obj);
  if (LLVM_UNLIKELY(iteratorRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_UNLIKELY(!(*iteratorRes)->isObject())) {
    return runtime.raiseTypeError("iterator is not an object");
  }
  auto iterator = runtime.makeHandle<JSObject>(std::move(*iteratorRes));

  CallResult<PseudoHandle<>> nextMethodRes = JSObject::getNamed_RJS(
      iterator, runtime, Predefined::getSymbolID(Predefined::next));
  if (LLVM_UNLIKELY(nextMethodRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return UncheckedIteratorRecord{
      iterator,
      Handle<HermesValue>::vmcast(
          runtime.makeHandle(std::move(*nextMethodRes)))};
}

CallResult<PseudoHandle<JSObject>> iteratorNext(
    Runtime &runtime,
    const CheckedIteratorRecord &iteratorRecord,
    llvh::Optional<Handle<>> value) {
  GCScopeMarkerRAII marker{runtime};

  auto resultRes = value
      ? Callable::executeCall1(
            iteratorRecord.nextMethod,
            runtime,
            iteratorRecord.iterator,
            value->getHermesValue())
      : Callable::executeCall0(
            iteratorRecord.nextMethod, runtime, iteratorRecord.iterator);
  if (LLVM_UNLIKELY(resultRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_UNLIKELY(!(*resultRes)->isObject())) {
    return runtime.raiseTypeError("iterator.next() did not return an object");
  }
  return PseudoHandle<JSObject>::vmcast(std::move(*resultRes));
}

CallResult<PseudoHandle<HermesValue>> iteratorValue(
    Runtime &runtime,
    Handle<JSObject> iterResult) {
  return JSObject::getNamed_RJS(
      iterResult, runtime, Predefined::getSymbolID(Predefined::value));
}

CallResult<Handle<JSObject>> iteratorStep(
    Runtime &runtime,
    const CheckedIteratorRecord &iteratorRecord) {
  auto resultRes = iteratorNext(runtime, iteratorRecord);
  if (LLVM_UNLIKELY(resultRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<JSObject> result = runtime.makeHandle(std::move(*resultRes));
  auto completeRes = JSObject::getNamed_RJS(
      result, runtime, Predefined::getSymbolID(Predefined::done));
  if (LLVM_UNLIKELY(completeRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (toBoolean(completeRes->get())) {
    return Runtime::makeNullHandle<JSObject>();
  }
  return result;
}

CallResult<bool> iteratorStepValue(
    Runtime &runtime,
    const CheckedIteratorRecord &iteratorRecord,
    PinnedValue<> *value) {
  // 1. Let result be ?IteratorStep(iteratorRecord)
  auto stepRes = iteratorStep(runtime, iteratorRecord);
  if (LLVM_UNLIKELY(stepRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 2. If result is DONE, then return DONE
  // In our implementation, return false when we hit the end of the iterator
  if (!*stepRes) {
    return false;
  }

  // 3. Let value be Completion(IteratorValue(result))
  // 4. If value is a throw completion, then
  //   a. Set iteratorRecord.[[Done]] to true
  auto valueRes = iteratorValue(runtime, *stepRes);
  if (LLVM_UNLIKELY(valueRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 5. Return ?value
  *value = std::move(*valueRes);
  return true;
}

ExecutionStatus iteratorClose(
    Runtime &runtime,
    Handle<JSObject> iterator,
    Handle<> completion) {
  ExecutionStatus completionStatus = completion->isEmpty()
      ? ExecutionStatus::RETURNED
      : ExecutionStatus::EXCEPTION;

  // 4. Let innerResult be GetMethod(iterator, "return").
  auto returnRes = getMethod(
      runtime,
      iterator,
      runtime.makeHandle(Predefined::getSymbolID(Predefined::returnStr)));

  // The only information we need to preserve is whether innerResult is an
  // exception, or if it contained an object. By default, initialize it to an
  // exception, which will be overwritten if resultRes is not an exception, and
  // the call to the closure it references does not throw.
  CallResult<bool> innerResultIsObject{ExecutionStatus::EXCEPTION};

  // 5. If innerResult.[[Type]] is normal, then
  if (LLVM_LIKELY(returnRes != ExecutionStatus::EXCEPTION)) {
    // a. Let return be innerResult.[[Value]].
    // b. If return is undefined, return Completion(completion).
    if (returnRes->getHermesValue().isUndefined()) {
      runtime.setThrownValue(*completion);
      return completionStatus;
    }
    Handle<Callable> returnFn =
        runtime.makeHandle(vmcast<Callable>(returnRes->getHermesValue()));
    // c. Set innerResult to Call(return, iterator).
    auto innerResultRes = Callable::executeCall0(returnFn, runtime, iterator);
    if (LLVM_UNLIKELY(innerResultRes == ExecutionStatus::EXCEPTION)) {
      if (isUncatchableError(runtime.getThrownValue())) {
        // If the call to return threw an uncatchable exception, that overrides
        // the completion, since the point of an uncatchable exception is to
        // prevent more JS from executing.
        return ExecutionStatus::EXCEPTION;
      }
    } else {
      innerResultIsObject = innerResultRes->getHermesValue().isObject();
    }
  }
  // Runtime::thrownValue now contains the innerResult's exception if it
  // was thrown.
  // GetMethod error here is deliberately deferred (no "?" in the spec).
  if (completionStatus == ExecutionStatus::EXCEPTION) {
    // 6. If completion.[[Type]] is throw, return Completion(completion).
    // Note: Overrides the innerResult exception.
    runtime.setThrownValue(*completion);
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_UNLIKELY(innerResultIsObject == ExecutionStatus::EXCEPTION)) {
    // 7. If innerResult.[[Type]] is throw, return Completion(innerResult).
    // Note: innerResult exception is still in Runtime::thrownValue,
    // so there is no need to set it again.
    return ExecutionStatus::EXCEPTION;
  }
  if (!*innerResultIsObject) {
    // 8. If Type(innerResult.[[Value]]) is not Object,
    //    throw a TypeError exception.
    return runtime.raiseTypeError("iterator.return() did not return an object");
  }
  return ExecutionStatus::RETURNED;
}

ExecutionStatus iteratorCloseAndRethrow(
    Runtime &runtime,
    Handle<JSObject> iterator) {
  struct : public Locals {
    PinnedValue<> completion;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.completion = runtime.getThrownValue();
  if (isUncatchableError(lv.completion.getHermesValue())) {
    // If an uncatchable exception was raised, do not swallow it, but instead
    // propagate it.
    return ExecutionStatus::EXCEPTION;
  }
  runtime.clearThrownValue();
  auto status = iteratorClose(runtime, iterator, lv.completion);
  (void)status;
  assert(
      status == ExecutionStatus::EXCEPTION && "exception swallowed mistakenly");
  return ExecutionStatus::EXCEPTION;
}

CallResult<Handle<JSArray>> iterableToArray(
    Runtime &runtime,
    Handle<HermesValue> items) {
  // IterableToList: 2a. Let iteratorRecord be ? GetIterator(items, sync).
  CallResult<CheckedIteratorRecord> iteratorRecordRes =
      getCheckedIterator(runtime, items);
  if (LLVM_UNLIKELY(iteratorRecordRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  CheckedIteratorRecord iteratorRecord = *iteratorRecordRes;

  // CreateArrayFromList: 1. Let array be ! ArrayCreate(0).
  auto arrRes = JSArray::create(runtime, 0, 0);
  assert(arrRes != ExecutionStatus::EXCEPTION && "could not create array");
  Handle<JSArray> array = runtime.makeHandle(std::move(*arrRes));
  // CreateArrayFromList: 2. Let n be 0.
  size_t n = 0;

  GCScopeMarkerRAII marker{runtime};
  for (;; marker.flush()) {
    // IterableToList: 5.a. Set next to ? IteratorStep(iteratorRecord).
    CallResult<Handle<JSObject>> nextRes =
        iteratorStep(runtime, iteratorRecord);
    if (LLVM_UNLIKELY(nextRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (!*nextRes) {
      break;
    }
    // 5.b.i. Let nextValue be ? IteratorValue(next).
    CallResult<PseudoHandle<HermesValue>> nextValueRes =
        iteratorValue(runtime, *nextRes);
    if (LLVM_UNLIKELY(nextValueRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // CreateArrayFromList: 3.a Perform ! CreateDataPropertyOrThrow(array, !
    // ToString(𝔽(n)), e).
    if (LLVM_UNLIKELY(
            JSArray::setElementAt(
                array,
                runtime,
                n,
                runtime.makeHandle(std::move(*nextValueRes))) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    // CreateArrayFromList: 3.b Set n to n + 1.
    n++;
  }
  if (LLVM_UNLIKELY(
          JSArray::setLengthProperty(array, runtime, n) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 4. Return array.
  return array;
}

bool isUncatchableError(HermesValue value) {
  if (auto *jsError = dyn_vmcast<JSError>(value)) {
    return !jsError->catchable();
  }
  return false;
}

Handle<JSObject>
createIterResultObject(Runtime &runtime, Handle<> value, bool done) {
  auto objHandle = runtime.makeHandle(JSObject::create(runtime));
  auto status = JSObject::defineOwnProperty(
      objHandle,
      runtime,
      Predefined::getSymbolID(Predefined::value),
      DefinePropertyFlags::getDefaultNewPropertyFlags(),
      value);
  (void)status;
  assert(
      status != ExecutionStatus::EXCEPTION && *status &&
      "put own value property cannot fail");
  status = JSObject::defineOwnProperty(
      objHandle,
      runtime,
      Predefined::getSymbolID(Predefined::done),
      DefinePropertyFlags::getDefaultNewPropertyFlags(),
      Runtime::getBoolValue(done));
  assert(
      status != ExecutionStatus::EXCEPTION && *status &&
      "put own value property cannot fail");
  return objHandle;
}

CallResult<Handle<Callable>> speciesConstructor(
    Handle<JSObject> O,
    Runtime &runtime,
    Handle<Callable> defaultConstructor) {
  // construct from the "constructor" property in self if that is defined, else
  // use the default one.
  auto res = JSObject::getNamed_RJS(
      O, runtime, Predefined::getSymbolID(Predefined::constructor));
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  PseudoHandle<> cons = std::move(*res);
  if (cons->isUndefined()) {
    return defaultConstructor;
  }
  if (!cons->isObject()) {
    return runtime.raiseTypeError(
        "Constructor must be an object if it is not undefined");
  }
  // There is no @@species (no Symbols yet), so we'll assume that there was
  // no other constructor specified.
  return defaultConstructor;
}

bool isConstructor(Runtime &runtime, HermesValue value) {
  return isConstructor(runtime, dyn_vmcast<Callable>(value));
}

bool isConstructor(Runtime &runtime, Callable *callable) {
  // This is not a complete definition, since ES6 and later define member
  // functions of objects to not be constructors; however, Hermes does not have
  // ES6 classes implemented yet, so we cannot check for that case.
  if (!callable) {
    return false;
  }

  // Traverse through BoundFunction & JSCallableProxy target chain to find the
  // eventual target.
  while (true) {
    if (auto *proxy = dyn_vmcast<JSCallableProxy>(callable)) {
      callable = proxy->getTarget(runtime);
      if (!callable) {
        return false;
      }
      continue;
    }
    if (auto *bound = dyn_vmcast<BoundFunction>(callable)) {
      callable = bound->getTarget(runtime);
      continue;
    }
    break;
  }

  // If it is a bytecode function, check the flags.
  if (auto *func = dyn_vmcast<JSFunction>(callable)) {
    return !func->getCodeBlock()->getHeaderFlags().isCallProhibited(true);
  }
  if (auto *nativeFunc = dyn_vmcast<NativeJSFunction>(callable)) {
    return nativeFunc->getFunctionInfo()->prohibit_invoke !=
        ProhibitInvoke::Construct;
  }

  // We check for NativeFunction since those are defined to not be
  // constructible, with the exception of NativeConstructor.
  if (!vmisa<NativeFunction>(callable) ||
      vmisa<FinalizableNativeFunction>(callable) ||
      vmisa<NativeConstructor>(callable)) {
    return true;
  }

  return false;
}

CallResult<PseudoHandle<JSObject>> ordinaryCreateFromConstructor_RJS(
    Runtime &runtime,
    Handle<Callable> constructor,
    Handle<JSObject> intrinsicDefaultProto) {
  struct : public Locals {
    PinnedValue<> proto;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  // 2. Let proto be ? Get(constructor, "prototype").
  auto protoRes = JSObject::getNamed_RJS(
      constructor, runtime, Predefined::getSymbolID(Predefined::prototype));
  if (LLVM_UNLIKELY(protoRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  lv.proto = std::move(*protoRes);

  // 3. If proto is not an Object, then
  // b. Set proto to realm's intrinsic object named intrinsicDefaultProto.
  // 4. Return proto.
  return JSObject::create(
      runtime,
      LLVM_LIKELY(lv.proto->isObject()) ? Handle<JSObject>::vmcast(&lv.proto)
                                        : intrinsicDefaultProto);
}

CallResult<bool>
ordinaryHasInstance(Runtime &runtime, Handle<> constructor, Handle<> object) {
  // 1. If IsCallable(C) is false, return false.
  if (!vmisa<Callable>(*constructor)) {
    return false;
  }

  Callable *ctor = vmcast<Callable>(*constructor);

  BoundFunction *bound;
  // 2. If C has a [[BoundTargetFunction]] internal slot, then
  while (LLVM_UNLIKELY(bound = dyn_vmcast<BoundFunction>(ctor))) {
    // 2a. Let BC be the value of C’s [[BoundTargetFunction]] internal slot.
    // 2b. Return InstanceofOperator(O,BC) (see 12.9.4).
    // Note that we can do this with the loop instead,
    // because bound->getTarget() must be a Callable, and Callables cannot
    // redefine @@hasInstance (non-configurable).
    // Callables call this function directly from their @@hasInstance
    // function.
    ctor = bound->getTarget(runtime);
  }

  // At this point 'ctor' is the actual function with a prototype.
  assert(ctor != nullptr && "ctor must not be null");

  // 3. If Type(O) is not Object, return false.
  if (LLVM_UNLIKELY(!object->isObject())) {
    return false;
  }

  // 4. Let P be Get(C, "prototype").
  auto propRes = JSObject::getNamed_RJS(
      runtime.makeHandle(ctor),
      runtime,
      Predefined::getSymbolID(Predefined::prototype));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 5. If Type(P) is not Object, throw a TypeError exception.
  Handle<JSObject> ctorPrototype = runtime.makeHandle(
      PseudoHandle<JSObject>::dyn_vmcast(std::move(*propRes)));
  if (LLVM_UNLIKELY(!ctorPrototype)) {
    return runtime.raiseTypeError(
        "function's '.prototype' is not an object in 'instanceof'");
  }

  // 6.1.7.3 Invariants of the Essential Internal Methods notes that
  // detection of infinite prototype chains is not enforceable as an
  // invariant if exotic objects exist in the chain.  Most of the
  // time, ScopedNativeDepthTracker will detect this. Here, we need to
  // check that we're not repeating forever.  Since ordinary object
  // chains are verified at the time the parent is set, we count Proxy
  // objects.  Thus, any length chain of ordinary objects is ok.
  constexpr unsigned int kMaxProxyCount = 1024;
  unsigned int proxyCount = 0;
  MutableHandle<JSObject> head{runtime, vmcast<JSObject>(object.get())};
  GCScopeMarkerRAII gcScope{runtime};
  // 6. Repeat
  while (true) {
    // 6a. Let O be O.[[GetPrototypeOf]]().
    CallResult<PseudoHandle<JSObject>> parentRes =
        JSObject::getPrototypeOf(head, runtime);
    if (LLVM_UNLIKELY(parentRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // 6b. If O is null, return false.
    if (!*parentRes) {
      return false;
    }
    // 6c. If SameValue(P, O) is true, return true.
    if (parentRes->get() == ctorPrototype.get()) {
      return true;
    }
    if (head->isProxyObject(runtime)) {
      ++proxyCount;
      if (proxyCount > kMaxProxyCount) {
        return runtime.raiseRangeError(
            "Maximum prototype chain length exceeded");
      }
    }
    head = parentRes->get();
    gcScope.flush();
  }
}

CallResult<bool> instanceOfOperator_RJS(
    Runtime &runtime,
    Handle<> object,
    Handle<> constructor) {
  // 1. If Type(C) is not Object, throw a TypeError exception.
  if (LLVM_UNLIKELY(!constructor->isObject())) {
    return runtime.raiseTypeError(
        "right operand of 'instanceof' is not an object");
  }

  // 2. Let instOfHandler be GetMethod(C,@@hasInstance).
  CallResult<PseudoHandle<>> instOfHandlerRes = JSObject::getNamed_RJS(
      Handle<JSObject>::vmcast(constructor),
      runtime,
      Predefined::getSymbolID(Predefined::SymbolHasInstance));
  if (LLVM_UNLIKELY(instOfHandlerRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (LLVM_LIKELY(
          (*instOfHandlerRes)->getRaw() ==
          runtime.functionPrototypeSymbolHasInstance.getHermesValue()
              .getRaw())) {
    // functionPrototypeSymbolHasInstance is just a passthrough to
    // ordinaryHasInstance. Avoid the extra function call.
    return ordinaryHasInstance(runtime, constructor, object);
  }

  auto instOfHandler = runtime.makeHandle(std::move(*instOfHandlerRes));

  // 4. If instOfHandler is not undefined, then
  if (!instOfHandler->isUndefined()) {
    // 5. Return ToBoolean(Call(instOfHandler, C, «O»)).
    if (!vmisa<Callable>(*instOfHandler)) {
      return runtime.raiseTypeError("instanceof handler must be callable");
    }
    auto callRes = Callable::executeCall1(
        Handle<Callable>::vmcast(instOfHandler), runtime, constructor, *object);
    if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return toBoolean(callRes->get());
  }

  // 6. If IsCallable(C) is false, throw a TypeError exception.
  if (!vmisa<Callable>(*constructor)) {
    return runtime.raiseTypeError(
        "right operand of 'instanceof' is not callable");
  }

  // 7. Return OrdinaryHasInstance(C, O).
  return ordinaryHasInstance(runtime, constructor, object);
}

/// ES6.0 7.2.8
/// Returns true if the object is a JSRegExp or has a Symbol.match property that
/// evaluates to true.
CallResult<bool> isRegExp(Runtime &runtime, Handle<> arg) {
  // 1. If Type(argument) is not Object, return false.
  if (!arg->isObject()) {
    return false;
  }
  Handle<JSObject> obj = Handle<JSObject>::vmcast(arg);
  // 2. Let isRegExp be Get(argument, @@match).
  auto propRes = JSObject::getNamed_RJS(
      obj, runtime, Predefined::getSymbolID(Predefined::SymbolMatch));
  // 3. ReturnIfAbrupt(isRegExp).
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 4. If isRegExp is not undefined, return ToBoolean(isRegExp).
  if (!(*propRes)->isUndefined()) {
    return toBoolean(propRes->get());
  }
  // 5. If argument has a [[RegExpMatcher]] internal slot, return true.
  // 6. Return false.
  return vmisa<JSRegExp>(arg.get());
}

CallResult<Handle<StringPrimitive>> symbolDescriptiveString(
    Runtime &runtime,
    Handle<SymbolID> sym) {
  // 1. Assert: Type(sym) is Symbol.
  // 2. Let desc be sym's [[Description]] value.
  // 3. If desc is undefined, set desc to the empty string.
  // 4. Assert: Type(desc) is String.
  auto desc = runtime.makeHandle<StringPrimitive>(
      runtime.getStringPrimFromSymbolID(*sym));
  SafeUInt32 descLen(desc->getStringLength());
  descLen.add(8);

  // 5. Return the string-concatenation of "Symbol(", desc, and ")".
  auto builder =
      StringBuilder::createStringBuilder(runtime, descLen, desc->isASCII());
  if (LLVM_UNLIKELY(builder == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  builder->appendASCIIRef({"Symbol(", 7});
  builder->appendStringPrim(desc);
  builder->appendCharacter(')');

  return builder->getStringPrimitive();
}

CallResult<bool> isArray(Runtime &runtime, JSObject *obj) {
  if (!obj) {
    return false;
  }
  while (true) {
    if (vmisa<JSArray>(obj)) {
      return true;
    }
    if (vmisa<FastArray>(obj)) {
      return true;
    }
    if (LLVM_LIKELY(!obj->isProxyObject(runtime))) {
      return false;
    }
    if (JSProxy::isRevoked(obj, runtime)) {
      return runtime.raiseTypeError("Proxy has been revoked");
    }
    obj = JSProxy::getTarget(obj, runtime).get();
    assert(obj && "target of non-revoked Proxy is null");
  }
}

CallResult<bool> isConcatSpreadable(Runtime &runtime, Handle<> value) {
  auto O = Handle<JSObject>::dyn_vmcast(value);
  if (!O) {
    return false;
  }

  CallResult<PseudoHandle<>> spreadable = JSObject::getNamed_RJS(
      O,
      runtime,
      Predefined::getSymbolID(Predefined::SymbolIsConcatSpreadable));
  if (LLVM_UNLIKELY(spreadable == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (!(*spreadable)->isUndefined()) {
    return toBoolean(spreadable->get());
  }

  return isArray(runtime, *O);
}

ExecutionStatus toPropertyDescriptor(
    Handle<> obj,
    Runtime &runtime,
    DefinePropertyFlags &flags,
    MutableHandle<> valueOrAccessor) {
  GCScopeMarkerRAII gcMarker{runtime};

  // Verify that the attributes argument is also an object.
  auto attributes = Handle<JSObject>::dyn_vmcast(obj);
  if (!attributes) {
    return runtime.raiseTypeError(
        "Object.defineProperty() Attributes argument is not an object");
  }

  NamedPropertyDescriptor desc;

  // Get enumerable property of the attributes.
  if (JSObject::getNamedDescriptorPredefined(
          attributes, runtime, Predefined::enumerable, desc)) {
    auto propRes = JSObject::getNamed_RJS(
        attributes,
        runtime,
        Predefined::getSymbolID(Predefined::enumerable),
        PropOpFlags().plusThrowOnError());
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    flags.enumerable = toBoolean(propRes->get());
    flags.setEnumerable = true;
  }

  // Get configurable property of the attributes.
  if (JSObject::getNamedDescriptorPredefined(
          attributes, runtime, Predefined::configurable, desc)) {
    auto propRes = JSObject::getNamed_RJS(
        attributes,
        runtime,
        Predefined::getSymbolID(Predefined::configurable),
        PropOpFlags().plusThrowOnError());
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    flags.configurable = toBoolean(propRes->get());
    flags.setConfigurable = true;
  }

  // Get value property of the attributes.
  if (JSObject::getNamedDescriptorPredefined(
          attributes, runtime, Predefined::value, desc)) {
    auto propRes = JSObject::getNamed_RJS(
        attributes,
        runtime,
        Predefined::getSymbolID(Predefined::value),
        PropOpFlags().plusThrowOnError());
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    valueOrAccessor.set(std::move(*propRes));
    flags.setValue = true;
  }

  // Get writable property of the attributes.
  if (JSObject::getNamedDescriptorPredefined(
          attributes, runtime, Predefined::writable, desc)) {
    auto propRes = JSObject::getNamed_RJS(
        attributes,
        runtime,
        Predefined::getSymbolID(Predefined::writable),
        PropOpFlags().plusThrowOnError());
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    flags.writable = toBoolean(propRes->get());
    flags.setWritable = true;
  }

  // Get getter property of the attributes.
  MutableHandle<Callable> getterPtr{runtime};
  if (JSObject::getNamedDescriptorPredefined(
          attributes, runtime, Predefined::get, desc)) {
    auto propRes = JSObject::getNamed_RJS(
        attributes,
        runtime,
        Predefined::getSymbolID(Predefined::get),
        PropOpFlags().plusThrowOnError());
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    flags.setGetter = true;
    PseudoHandle<> getter = std::move(*propRes);
    if (LLVM_LIKELY(!getter->isUndefined())) {
      getterPtr = dyn_vmcast<Callable>(getter.get());
      if (LLVM_UNLIKELY(!getterPtr)) {
        return runtime.raiseTypeError(
            "Invalid property descriptor. Getter must be a function.");
      }
    }
  }

  // Get setter property of the attributes.
  MutableHandle<Callable> setterPtr{runtime};
  if (JSObject::getNamedDescriptorPredefined(
          attributes, runtime, Predefined::set, desc)) {
    auto propRes = JSObject::getNamed_RJS(
        attributes,
        runtime,
        Predefined::getSymbolID(Predefined::set),
        PropOpFlags().plusThrowOnError());
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    flags.setSetter = true;
    PseudoHandle<> setter = std::move(*propRes);
    if (LLVM_LIKELY(!setter->isUndefined())) {
      setterPtr = PseudoHandle<Callable>::dyn_vmcast(std::move(setter));
      if (LLVM_UNLIKELY(!setterPtr)) {
        return runtime.raiseTypeError(
            "Invalid property descriptor. Setter must be a function.");
      }
    }
  }

  // Construct property accessor if getter/setter is set.
  if (flags.setSetter || flags.setGetter) {
    if (flags.setValue) {
      return runtime.raiseTypeError(
          "Invalid property descriptor. Can't set both accessor and value.");
    }
    if (flags.setWritable) {
      return runtime.raiseTypeError(
          "Invalid property descriptor. Can't set both accessor and writable.");
    }
    valueOrAccessor.set(
        PropertyAccessor::create(runtime, getterPtr, setterPtr)
            .getHermesValue());
  }

  return ExecutionStatus::RETURNED;
}

CallResult<HermesValue> objectFromPropertyDescriptor(
    Runtime &runtime,
    DefinePropertyFlags dpFlags,
    Handle<> valueOrAccessor) {
  Handle<JSObject> obj = runtime.makeHandle(JSObject::create(runtime));

  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();

  if (!dpFlags.isAccessor()) {
    // Data Descriptor
    if (dpFlags.setValue) {
      auto result = JSObject::defineOwnProperty(
          obj,
          runtime,
          Predefined::getSymbolID(Predefined::value),
          dpf,
          valueOrAccessor,
          PropOpFlags().plusThrowOnError());
      assert(
          result != ExecutionStatus::EXCEPTION &&
          "defineOwnProperty() failed on a new object");
      if (result == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
    }

    if (dpFlags.setWritable) {
      auto result = JSObject::defineOwnProperty(
          obj,
          runtime,
          Predefined::getSymbolID(Predefined::writable),
          dpf,
          Runtime::getBoolValue(dpFlags.writable),
          PropOpFlags().plusThrowOnError());
      assert(
          result != ExecutionStatus::EXCEPTION &&
          "defineOwnProperty() failed on a new object");
      if (result == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
    }
  } else {
    // Accessor
    if (dpFlags.setGetter) {
      auto *accessor = vmcast<PropertyAccessor>(valueOrAccessor.get());
      auto getter = runtime.makeHandle(
          accessor->getter ? HermesValue::encodeObjectValue(
                                 accessor->getter.getNonNull(runtime))
                           : HermesValue::encodeUndefinedValue());

      auto result = JSObject::defineOwnProperty(
          obj,
          runtime,
          Predefined::getSymbolID(Predefined::get),
          dpf,
          getter,
          PropOpFlags().plusThrowOnError());
      assert(
          result != ExecutionStatus::EXCEPTION &&
          "defineOwnProperty() failed on a new object");
      if (result == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
    }

    if (dpFlags.setSetter) {
      auto *accessor = vmcast<PropertyAccessor>(valueOrAccessor.get());
      auto setter = runtime.makeHandle(
          accessor->setter ? HermesValue::encodeObjectValue(
                                 accessor->setter.getNonNull(runtime))
                           : HermesValue::encodeUndefinedValue());

      auto result = JSObject::defineOwnProperty(
          obj,
          runtime,
          Predefined::getSymbolID(Predefined::set),
          dpf,
          setter,
          PropOpFlags().plusThrowOnError());
      assert(
          result != ExecutionStatus::EXCEPTION &&
          "defineOwnProperty() failed on a new object");
      if (result == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
    }
  }

  if (dpFlags.setEnumerable) {
    auto result = JSObject::defineOwnProperty(
        obj,
        runtime,
        Predefined::getSymbolID(Predefined::enumerable),
        dpf,
        Runtime::getBoolValue(dpFlags.enumerable),
        PropOpFlags().plusThrowOnError());
    assert(
        result != ExecutionStatus::EXCEPTION &&
        "defineOwnProperty() failed on a new object");
    if (result == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
  }

  if (dpFlags.setConfigurable) {
    auto result = JSObject::defineOwnProperty(
        obj,
        runtime,
        Predefined::getSymbolID(Predefined::configurable),
        dpf,
        Runtime::getBoolValue(dpFlags.configurable),
        PropOpFlags().plusThrowOnError());
    assert(
        result != ExecutionStatus::EXCEPTION &&
        "defineOwnProperty() failed on a new object");
    if (result == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return obj.getHermesValue();
}

bool isIntegralNumber(double number) {
  // 1. if Type(argument) is not Number, return false
  // it is a number

  // 2. if argument is NaN, +inf, -inf, return false
  if (std::isnan(number) || number == std::numeric_limits<double>::infinity() ||
      number == -std::numeric_limits<double>::infinity()) {
    return false;
  }

  // 3. if floor(abs(R(argument))) != abs(R(argument)) return false
  if (std::floor(std::abs(number)) != std::abs(number)) {
    return false;
  }

  // 4. return true
  return true;
}

CallResult<HermesValue> toBigInt_RJS(Runtime &runtime, Handle<> value) {
  auto prim = toPrimitive_RJS(runtime, value, PreferredType::NUMBER);
  if (LLVM_UNLIKELY(prim == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  switch (prim->getETag()) {
    default:
      break;
    case HermesValue::ETag::Undefined:
      return runtime.raiseTypeError("invalid argument to BigInt()");
    case HermesValue::ETag::Null:
      return runtime.raiseTypeError("invalid argument to BigInt()");
    case HermesValue::ETag::Bool:
      return BigIntPrimitive::fromSigned(runtime, prim->getBool() ? 1 : 0);
    case HermesValue::ETag::BigInt1:
    case HermesValue::ETag::BigInt2:
      return *prim;
    case HermesValue::ETag::Str1:
    case HermesValue::ETag::Str2: {
      auto n = stringToBigInt(runtime, runtime.makeHandle(*prim));
      if (LLVM_UNLIKELY(n == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      if (n->isUndefined()) {
        return runtime.raiseSyntaxError("can't convert string to bigint");
      }
      return *n;
    }
    case HermesValue::ETag::Symbol:
      return runtime.raiseTypeError("invalid argument to BigInt()");
  }

  return runtime.raiseTypeError("invalid argument to BigInt()");
}

CallResult<HermesValue> stringToBigInt(Runtime &runtime, Handle<> value) {
  if (value->isString()) {
    auto str = value->getString();

    std::string outError;
    auto parsedBigInt = str->isASCII()
        ? bigint::ParsedBigInt::parsedBigIntFromStringIntegerLiteral(
              str->getStringRef<char>(), &outError)
        : bigint::ParsedBigInt::parsedBigIntFromStringIntegerLiteral(
              str->getStringRef<char16_t>(), &outError);
    if (!parsedBigInt) {
      return HermesValue::encodeUndefinedValue();
    }

    return BigIntPrimitive::fromBytes(runtime, parsedBigInt->getBytes());
  }

  return runtime.raiseTypeError("Invalid argument to stringToBigInt");
}

CallResult<HermesValue> thisBigIntValue(Runtime &runtime, Handle<> value) {
  if (value->isBigInt())
    return *value;
  if (auto *jsBigInt = dyn_vmcast<JSBigInt>(*value))
    return HermesValue::encodeBigIntValue(
        JSBigInt::getPrimitiveBigInt(jsBigInt, runtime));
  return runtime.raiseTypeError("value is not a bigint");
}

ExecutionStatus setTemplateObjectProps(
    Runtime &runtime,
    Handle<JSObject> templateObj,
    Handle<JSObject> rawObj) {
  // Make 'length' property on the raw object read-only.
  DefinePropertyFlags readOnlyDPF{};
  readOnlyDPF.setWritable = 1;
  readOnlyDPF.setConfigurable = 1;
  readOnlyDPF.writable = 0;
  readOnlyDPF.configurable = 0;
  auto readOnlyRes = JSObject::defineOwnProperty(
      rawObj,
      runtime,
      Predefined::getSymbolID(Predefined::length),
      readOnlyDPF,
      Runtime::getUndefinedValue(),
      PropOpFlags().plusThrowOnError());
  if (LLVM_UNLIKELY(readOnlyRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_UNLIKELY(!*readOnlyRes)) {
    return runtime.raiseTypeError(
        "Failed to set 'length' property on the raw object read-only.");
  }
  JSObject::preventExtensionsNonProxy(rawObj, runtime);

  // Set raw object as a read-only non-enumerable property of the template
  // object.
  PropertyFlags constantPF{};
  constantPF.writable = 0;
  constantPF.configurable = 0;
  constantPF.enumerable = 0;
  auto putNewRes = JSObject::defineNewOwnProperty(
      templateObj,
      runtime,
      Predefined::getSymbolID(Predefined::raw),
      constantPF,
      rawObj);
  if (LLVM_UNLIKELY(putNewRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // Make 'length' property on the template object read-only.
  readOnlyRes = JSObject::defineOwnProperty(
      templateObj,
      runtime,
      Predefined::getSymbolID(Predefined::length),
      readOnlyDPF,
      Runtime::getUndefinedValue(),
      PropOpFlags().plusThrowOnError());
  if (LLVM_UNLIKELY(readOnlyRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_UNLIKELY(!*readOnlyRes)) {
    return runtime.raiseTypeError(
        "Failed to set 'length' property on the raw object read-only.");
  }
  JSObject::preventExtensionsNonProxy(templateObj, runtime);

  return ExecutionStatus::RETURNED;
}

ExecutionStatus getSetRecord(
    Runtime &runtime,
    Handle<> obj,
    double *size,
    PinnedValue<Callable> *hasMethod,
    PinnedValue<Callable> *keysMethod) {
  // 1. If obj is not an Object, throw a TypeError exception
  if (LLVM_UNLIKELY(!vmisa<JSObject>(*obj))) {
    return runtime.raiseTypeError("getSetRecord argument is not an Object");
  }
  struct : public Locals {
    PinnedValue<> tmp;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  // 2. Let rawSize be ?Get(obj, "size")
  auto rawSizeRes = JSObject::getNamed_RJS(
      Handle<JSObject>::vmcast(obj),
      runtime,
      Predefined::getSymbolID(Predefined::size));
  if (LLVM_UNLIKELY(rawSizeRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.tmp = std::move(*rawSizeRes);

  // 3. Let numSize be ?ToNumber(rawSize)
  // 4. NOTE: If rawSize is undefined, then numSize will be NaN.
  auto numSizeRes = toNumber_RJS(runtime, lv.tmp);
  if (LLVM_UNLIKELY(numSizeRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.tmp = std::move(*numSizeRes);
  // 5. If numSize is NaN, throw a TypeError exception
  if (lv.tmp->isNaN()) {
    return runtime.raiseTypeError("Size of the object cannot be NaN");
  }
  // 6. Let intSize be !ToIntegerOrInfinity(numSize)
  auto intSizeRes = toIntegerOrInfinity(runtime, lv.tmp);
  if (LLVM_UNLIKELY(intSizeRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  lv.tmp = std::move(*intSizeRes);
  // 7. If intSize < 0, throw a RangeError
  if (lv.tmp->getNumber() < 0) {
    return runtime.raiseRangeError("Size of the object cannot be negative");
  }
  *size = lv.tmp->getNumber();

  // 8. Let has be ?Get(obj, "has")
  auto hasRes = JSObject::getNamed_RJS(
      Handle<JSObject>::vmcast(obj),
      runtime,
      Predefined::getSymbolID(Predefined::has));
  if (hasRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.tmp = std::move(*hasRes);
  // 9. If IsCallable(has) is false, throw a TypeError exception
  if (LLVM_UNLIKELY(!vmisa<Callable>(*lv.tmp))) {
    return runtime.raiseTypeError(
        "has property of the object must be callable");
  }
  *hasMethod = vmcast<Callable>(*lv.tmp);

  // 10. Let has be ?Get(obj, "keys")
  auto keysRes = JSObject::getNamed_RJS(
      Handle<JSObject>::vmcast(obj),
      runtime,
      Predefined::getSymbolID(Predefined::keys));
  if (keysRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.tmp = std::move(*keysRes);
  // 11. If IsCallable(keys) is false, throw a TypeError exception
  if (LLVM_UNLIKELY(!vmisa<Callable>(*lv.tmp))) {
    return runtime.raiseTypeError(
        "keys property of the object must be callable");
  }
  *keysMethod = vmcast<Callable>(*lv.tmp);

  return ExecutionStatus::RETURNED;
}

namespace {

enum class KeyCoercion { Property, Collection };

/// ES15.0 7.3.35 GroupBy ( items, callback, keyCoercion )
/// https://tc39.es/ecma262/#sec-groupby
/// Groups a list of items into an Object based on the results of a callback
/// which operates on each item.
/// \param[out] target the JSObject to which to add keyed groups if keyCoercion
/// is `Property` or a JSMap to which to add keyed groups if keyCoercion is
/// `Collection`. The keyed groups must always be of type JSArray.
/// \param items JSArray of ECMAValues to be grouped.
/// \param callback callback which returns the key per given item.
/// \return a JSObject if keyCoercion is `Property` or a JSMap if keyCoercion
/// is `Collection`.
template <KeyCoercion keyCoercion, class T>
ExecutionStatus
groupBy(Runtime &runtime, Handle<T> target, Handle<> items, Handle<> callback) {
  struct : public Locals {
    PinnedValue<> key;
    PinnedValue<JSArray> value;
    PinnedValue<> tmpValueMaybeArray;
    PinnedValue<> next;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 1. Perform ? RequireObjectCoercible(items).
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, items) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 2. If IsCallable(callback) is false, throw a TypeError exception.
  if (LLVM_UNLIKELY(!vmisa<Callable>(*callback))) {
    return runtime.raiseTypeError("callback must be Callable in groupby");
  }

  // 3. Let groups be a new empty List.
  // 4. Let iteratorRecord be ? GetIterator(items, sync).
  CallResult<CheckedIteratorRecord> iteratorRecordResult =
      getCheckedIterator(runtime, items);

  if (LLVM_UNLIKELY(iteratorRecordResult == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  const CheckedIteratorRecord &iteratorRecord = *iteratorRecordResult;

  GCScope gcScope(runtime);
  auto marker = gcScope.createMarker();

  // 5. Let k be 0.
  uint64_t k = 0;

  // 6. Repeat,
  while (true) {
    // a. If k ≥ 2**53 - 1, then
    if (LLVM_UNLIKELY(k >= ((uint64_t)1 << 53) - 1)) {
      // i. Let error be ThrowCompletion(a newly created TypeError object).
      (void)runtime.raiseTypeError("groupBy result out of space");

      // ii. Return ? IteratorClose(iteratorRecord, error).
      return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
    }

    // b. Let next be ? IteratorStepValue(iteratorRecord).
    auto stepValRes = iteratorStepValue(runtime, iteratorRecord, &lv.next);

    if (LLVM_UNLIKELY(stepValRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // c. If next is done, then
    if (!*stepValRes) {
      // i. Return groups.
      break;
    }

    // d. Let value be next.
    // e. Let key be Completion(Call(callback, undefined, « value, 𝔽(k) »)).
    auto keyRes = Callable::executeCall2(
        Handle<Callable>::vmcast(callback),
        runtime,
        Runtime::getUndefinedValue(),
        lv.next.getHermesValue(),
        HermesValue::encodeTrustedNumberValue(k));

    // f. IfAbruptCloseIterator(key, iteratorRecord).
    if (LLVM_UNLIKELY(keyRes == ExecutionStatus::EXCEPTION)) {
      return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
    }

    lv.key = std::move(*keyRes);

    // g. If keyCoercion is PROPERTY, then
    if constexpr (keyCoercion == KeyCoercion::Property) {
      // i. Set key to Completion(ToPropertyKey(key)).
      auto keyRes = toPropertyKey(runtime, lv.key);

      // ii. IfAbruptCloseIterator(key, iteratorRecord).
      if (LLVM_UNLIKELY(keyRes == ExecutionStatus::EXCEPTION)) {
        return iteratorCloseAndRethrow(runtime, iteratorRecord.iterator);
      }

      lv.key = std::move(*keyRes);
    }
    // h. Else,
    else {
      // i. Assert: keyCoercion is COLLECTION.
      static_assert(
          keyCoercion == KeyCoercion::Collection,
          "keyCoercion must be of type `Collection`");

      // ii. Set key to CanonicalizeKeyedCollectionKey(key).
      auto key = canonicalizeKeyedCollectionKey(*lv.key);

      lv.key = std::move(key);
    }

    // i. Perform AddValueToKeyedGroup(groups, key, value).
    // We deviate from the spec here for performance reasons by acting on the
    // resulting Map or Object directly, rather than creating an intermediate
    // hashmap.
    //
    // Get the keyed group from the target if it exists, otherwise return
    // undefined.
    if constexpr (keyCoercion == KeyCoercion::Property) {
      auto propValueRes = JSObject::getComputed_RJS(
          Handle<JSObject>::vmcast(target), runtime, lv.key);

      if (LLVM_UNLIKELY(propValueRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }

      lv.tmpValueMaybeArray = std::move(*propValueRes);
    } else {
      static_assert(
          keyCoercion == KeyCoercion::Collection,
          "keyCoercion must be of type `Collection`");

      lv.tmpValueMaybeArray = Handle<JSMap>::vmcast(target)
                                  ->get(runtime, *lv.key)
                                  .unboxToHV(runtime);
    }

    if (lv.tmpValueMaybeArray->isUndefined()) {
      // If a keyed group does not exist, add key to the result object and
      // initialize with an empty array.
      auto emptyArrRes = JSArray::create(runtime, 0, 0);

      if (LLVM_UNLIKELY(emptyArrRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }

      lv.value = std::move(*emptyArrRes);

      // Add empty array to keyed group property key.
      if constexpr (keyCoercion == KeyCoercion::Property) {
        auto newArrayRes = JSObject::putComputed_RJS(
            Handle<JSObject>::vmcast(target),
            runtime,
            lv.key,
            lv.value,
            PropOpFlags().plusThrowOnError());

        if (LLVM_UNLIKELY(newArrayRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
      } else {
        static_assert(
            keyCoercion == KeyCoercion::Collection,
            "keyCoercion must be of type `Collection`");

        auto newArrayRes = JSMap::insert(
            Handle<JSMap>::vmcast(target), runtime, lv.key, lv.value);

        if (LLVM_UNLIKELY(newArrayRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
      }
    } else {
      lv.value = Handle<JSArray>::vmcast(&lv.tmpValueMaybeArray);
    }

    size_t keyedGroupLength = JSArray::getLength(*lv.value, runtime);

    // Append keyed group array with new item that matches the key.
    auto appendItemRes =
        JSArray::setElementAt(lv.value, runtime, keyedGroupLength, lv.next);

    if (LLVM_UNLIKELY(appendItemRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    keyedGroupLength++;

    // Increase the length of keyed group array.
    if (LLVM_UNLIKELY(
            JSArray::setLengthProperty(lv.value, runtime, keyedGroupLength) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    gcScope.flushToMarker(marker);

    // j. Set k to k + 1.
    k++;
  }

  return ExecutionStatus::RETURNED;
}

} // namespace

ExecutionStatus groupByProperty(
    Runtime &runtime,
    Handle<JSObject> target,
    Handle<> items,
    Handle<> callback) {
  return groupBy<KeyCoercion::Property>(runtime, target, items, callback);
};

ExecutionStatus groupByCollection(
    Runtime &runtime,
    Handle<JSMapImpl<CellKind::JSMapKind>> target,
    Handle<> items,
    Handle<> callback) {
  return groupBy<KeyCoercion::Collection>(runtime, target, items, callback);
};

} // namespace vm
} // namespace hermes

//======= Static Hermes =======

using namespace hermes::vm;

extern "C" SHLegacyValue _sh_ljs_to_property_key(
    SHRuntime *shr,
    const SHLegacyValue *val) {
  Runtime &runtime = getRuntime(shr);
  CallResult<HermesValue> ret{ExecutionStatus::EXCEPTION};
  {
    GCScopeMarkerRAII marker{runtime};
    ret =
        toPropertyKey(runtime, Handle<>(toPHV(val))).toCallResultHermesValue();
  }
  if (LLVM_UNLIKELY(ret == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *ret;
}

extern "C" double _sh_ljs_to_double_rjs(
    SHRuntime *shr,
    const SHLegacyValue *n) {
  auto *pn = toPHV(n);
  if (LLVM_LIKELY(pn->isNumber()))
    return pn->getNumber();
  Runtime &runtime = getRuntime(shr);
  CallResult<HermesValue> cr{ExecutionStatus::EXCEPTION};
  {
    GCScopeMarkerRAII marker{runtime};
    cr = toNumber_RJS(runtime, Handle<>::vmcast(pn));
  }
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return cr->getDouble();
}

extern "C" SHLegacyValue _sh_ljs_to_numeric_rjs(
    SHRuntime *shr,
    const SHLegacyValue *n) {
  auto *pn = toPHV(n);
  if (LLVM_LIKELY(pn->isNumber()) || LLVM_LIKELY(pn->isBigInt()))
    return *pn;
  Runtime &runtime = getRuntime(shr);
  CallResult<HermesValue> cr{ExecutionStatus::EXCEPTION};
  {
    GCScopeMarkerRAII marker{runtime};
    cr = toNumeric_RJS(runtime, Handle<>::vmcast(pn));
  }
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *cr;
}

extern "C" double _sh_ljs_to_int32_rjs(SHRuntime *shr, const SHLegacyValue *n) {
  auto *pn = toPHV(n);
  if (LLVM_LIKELY(pn->isNumber()))
    return hermes::truncateToInt32(pn->getNumber());
  Runtime &runtime = getRuntime(shr);
  CallResult<HermesValue> cr{ExecutionStatus::EXCEPTION};
  {
    GCScopeMarkerRAII marker{runtime};
    cr = toInt32_RJS(runtime, Handle<>::vmcast(pn));
  }
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return cr->getDouble();
}

extern "C" double _sh_ljs_to_uint32_rjs(
    SHRuntime *shr,
    const SHLegacyValue *n) {
  auto *pn = toPHV(n);
  uint32_t argInt;
  if (LLVM_LIKELY(_sh_ljs_tryfast_truncate_to_uint32(*n, &argInt))) {
    return (double)argInt;
  }
  Runtime &runtime = getRuntime(shr);
  CallResult<HermesValue> cr{ExecutionStatus::EXCEPTION};
  {
    GCScopeMarkerRAII marker{runtime};
    cr = toUInt32_RJS(runtime, Handle<>::vmcast(pn));
  }
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return cr->getDouble();
}

#define SH_COMPARISON_OP(name, call, oper)                              \
  extern "C" bool name(                                                 \
      SHRuntime *shr, const SHLegacyValue *a, const SHLegacyValue *b) { \
    auto *pa = toPHV(a);                                                \
    auto *pb = toPHV(b);                                                \
    if (LLVM_LIKELY(_sh_ljs_are_both_non_nan_numbers(*a, *b)))          \
      return pa->getNumber() oper pb->getNumber();                      \
    Runtime &runtime = getRuntime(shr);                                 \
    CallResult<bool> cr{false};                                         \
    {                                                                   \
      GCScopeMarkerRAII marker{runtime};                                \
      cr = call(runtime, Handle<>::vmcast(pa), Handle<>::vmcast(pb));   \
    }                                                                   \
    if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))                \
      _sh_throw_current(shr);                                           \
    return *cr;                                                         \
  }

SH_COMPARISON_OP(_sh_ljs_less_rjs, lessOp_RJS, <);
SH_COMPARISON_OP(_sh_ljs_greater_rjs, greaterOp_RJS, >);
SH_COMPARISON_OP(_sh_ljs_less_equal_rjs, lessEqualOp_RJS, <=);
SH_COMPARISON_OP(_sh_ljs_greater_equal_rjs, greaterEqualOp_RJS, >=);

/// Perform JS addition with a fastpath for doubles.
extern "C" SHLegacyValue _sh_ljs_add_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  auto *pa = toPHV(a);
  auto *pb = toPHV(b);
  if (LLVM_LIKELY(_sh_ljs_are_both_non_nan_numbers(*a, *b)))
    return HermesValue::encodeTrustedNumberValue(
        pa->getNumber() + pb->getNumber());
  Runtime &runtime = getRuntime(shr);
  CallResult<HermesValue> cr{ExecutionStatus::EXCEPTION};
  {
    GCScopeMarkerRAII marker{runtime};
    cr = addOp_RJS(runtime, Handle<>::vmcast(pa), Handle<>::vmcast(pb));
  }
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *cr;
}

namespace {
inline double doDiv(double x, double y)
    LLVM_NO_SANITIZE("float-divide-by-zero");
double doDiv(double x, double y) {
  // UBSan will complain about float divide by zero as our implementation
  // of division depends on IEEE 754 float divide by zero. All modern
  // compilers implement this and there is no trivial work-around without
  // sacrificing performance and readability.
  return x / y;
}

inline double doMod(double x, double y) {
  // We use fmod here for simplicity. Theoretically fmod behaves slightly
  // differently than the ECMAScript Spec. fmod applies round-towards-zero for
  // the remainder when it's not representable by a double; while the spec
  // requires round-to-nearest. As an example, 5 % 0.7 will give
  // 0.10000000000000031 using fmod, but using the rounding style described by
  // the spec, the output should really be 0.10000000000000053. Such difference
  // can be ignored in practice.
  return std::fmod(x, y);
}

inline double doMul(double x, double y) {
  return x * y;
}

inline double doSub(double x, double y) {
  return x - y;
}

inline double doInc(double d) {
  return d + 1;
}

inline double doDec(double d) {
  return d - 1;
}

inline int32_t doBitAnd(int32_t x, int32_t y) {
  return x & y;
}

inline int32_t doBitOr(int32_t x, int32_t y) {
  return x | y;
}

inline int32_t doBitXor(int32_t x, int32_t y) {
  return x ^ y;
}

/// \return Math.imul(x, y): the 32-bit integer product of x and y, computed
/// by multiplying the operands as unsigned 32-bit values (to avoid signed
/// overflow UB) and reinterpreting the result as signed.
inline int32_t doImul(int32_t x, int32_t y) {
  uint32_t product = static_cast<uint32_t>(x) * static_cast<uint32_t>(y);
  return static_cast<int32_t>(product);
}

inline int32_t doLShift(uint32_t x, uint32_t y) {
  return x << y;
}

inline int32_t doRShift(int32_t x, uint32_t y) {
  return x >> y;
}

inline uint32_t doURShift(uint32_t x, uint32_t y) {
  return x >> y;
}

using BigIntBinaryOp = CallResult<HermesValue>(
    Runtime &,
    Handle<BigIntPrimitive>,
    Handle<BigIntPrimitive>);

CallResult<HermesValue> doBigIntBinOp(
    Runtime &runtime,
    BigIntBinaryOp Oper,
    Handle<BigIntPrimitive> lhs,
    Handle<> rhs) {
  // Cannot use ToBigInt here as it would incorrectly allow boolean/string rhs.
  CallResult<HermesValue> res = toNumeric_RJS(runtime, rhs);
  if (res == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  if (!res->isBigInt()) {
    return runtime.raiseTypeErrorForValue("Cannot convert ", rhs, " to BigInt");
  }
  return Oper(runtime, lhs, runtime.makeHandle(res->getBigInt()));
}

template <auto Oper, auto BigIntOper>
SHLegacyValue
binOpImpl(SHRuntime *shr, const SHLegacyValue *a, const SHLegacyValue *b) {
  Handle<> lhs{toPHV(a)}, rhs{toPHV(b)};
  // Fast path, both arguments are numbers.
  if (LLVM_LIKELY(_sh_ljs_are_both_non_nan_numbers(*a, *b)))
    return HermesValue::encodeTrustedNumberValue(
        Oper(lhs->getNumber(), rhs->getNumber()));

  auto res = [&]() -> CallResult<HermesValue> {
    Runtime &runtime = getRuntime(shr);
    GCScopeMarkerRAII marker{runtime};
    // Try converting the LHS to a primitive.
    auto lPrimRes = toPrimitive_RJS(runtime, lhs, PreferredType::NUMBER);
    if (LLVM_UNLIKELY(lPrimRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    if (LLVM_LIKELY(!lPrimRes->isBigInt())) {
      // If the LHS is not a BigInt, then perform a number operation, and throw
      // if that fails.
      auto lNumRes = toNumber_RJS(runtime, runtime.makeHandle(*lPrimRes));
      if (LLVM_UNLIKELY(lNumRes == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      auto rNumRes = toNumber_RJS(runtime, rhs);
      if (LLVM_UNLIKELY(rNumRes == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      return HermesValue::encodeTrustedNumberValue(
          Oper(lNumRes->getDouble(), rNumRes->getDouble()));
    }
    // LHS is a BigInt, try to convert RHS to a BigInt as well and perform a
    // BigInt operation.
    return doBigIntBinOp(
        runtime, BigIntOper, runtime.makeHandle(lPrimRes->getBigInt()), rhs);
  }();
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *res;
}

template <auto Oper, auto BigIntOper>
SHLegacyValue incDecOperImpl(SHRuntime *shr, const SHLegacyValue *n) {
  Handle<> nHandle{toPHV(n)};
  if (LLVM_LIKELY(nHandle->isNumber()))
    return HermesValue::encodeTrustedNumberValue(Oper(nHandle->getNumber()));

  auto res = [&]() -> CallResult<HermesValue> {
    Runtime &runtime = getRuntime(shr);
    GCScopeMarkerRAII marker{runtime};

    // Try converting the argument to a primitive.
    auto primRes = toPrimitive_RJS(runtime, nHandle, PreferredType::NUMBER);
    if (LLVM_UNLIKELY(primRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;

    // If the primitive is not a BigInt, perform a normal number inc/dec.
    if (LLVM_LIKELY(!primRes->isBigInt())) {
      auto numRes = toNumber_RJS(runtime, runtime.makeHandle(*primRes));
      if (LLVM_UNLIKELY(numRes == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      return HermesValue::encodeTrustedNumberValue(Oper(numRes->getNumber()));
    }

    return BigIntOper(runtime, runtime.makeHandle(primRes->getBigInt()));
  }();
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *res;
}

template <auto Oper, auto BigIntOper>
SHLegacyValue
bitOperImpl(SHRuntime *shr, const SHLegacyValue *a, const SHLegacyValue *b) {
  Handle<> lhs{toPHV(a)}, rhs{toPHV(b)};
  // Fast path, both arguments are numbers.
  if (LLVM_LIKELY(_sh_ljs_are_both_non_nan_numbers(*a, *b)))
    return HermesValue::encodeTrustedNumberValue(Oper(
        hermes::truncateToInt32(lhs->getNumber()),
        hermes::truncateToInt32(rhs->getNumber())));
  auto res = [&]() -> CallResult<HermesValue> {
    Runtime &runtime = getRuntime(shr);
    GCScopeMarkerRAII marker{runtime};
    // Try converting the LHS to a primitive.
    auto lPrimRes = toPrimitive_RJS(runtime, lhs, PreferredType::NUMBER);
    if (LLVM_UNLIKELY(lPrimRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    if (LLVM_LIKELY(!lPrimRes->isBigInt())) {
      // If the LHS is not a BigInt, then perform a number operation, and throw
      // if that fails.
      auto lIntRes = toInt32_RJS(runtime, runtime.makeHandle(*lPrimRes));
      if (LLVM_UNLIKELY(lIntRes == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      auto rIntRes = toInt32_RJS(runtime, std::move(rhs));
      if (LLVM_UNLIKELY(rIntRes == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      return HermesValue::encodeTrustedNumberValue(Oper(
          lIntRes->getNumberAs<int32_t>(), rIntRes->getNumberAs<int32_t>()));
    }
    // LHS is a BigInt, try to convert RHS to a BigInt as well and perform a
    // BigInt operation.
    return doBigIntBinOp(
        runtime, BigIntOper, runtime.makeHandle(lPrimRes->getBigInt()), rhs);
  }();
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *res;
}

template <auto Oper, auto ToIntegral, auto BigIntOper>
SHLegacyValue
shiftOperImpl(SHRuntime *shr, const SHLegacyValue *a, const SHLegacyValue *b) {
  Handle<> lhs{toPHV(a)}, rhs{toPHV(b)};
  // Fast path, both arguments are numbers.
  if (LLVM_LIKELY(_sh_ljs_are_both_non_nan_numbers(*a, *b))) {
    auto lnum = hermes::truncateToInt32(lhs->getNumber());
    uint32_t rnum = hermes::truncateToInt32(rhs->getNumber()) & 0x1f;
    return HermesValue::encodeTrustedNumberValue(Oper(lnum, rnum));
  }
  auto res = [&]() -> CallResult<HermesValue> {
    Runtime &runtime = getRuntime(shr);
    GCScopeMarkerRAII marker{runtime};
    // Try converting the LHS to a primitive.
    auto lPrimRes =
        toPrimitive_RJS(runtime, std::move(lhs), PreferredType::NUMBER);
    if (LLVM_UNLIKELY(lPrimRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    if (LLVM_LIKELY(!lPrimRes->isBigInt())) {
      // If the LHS is not a BigInt, then perform a number operation, and throw
      // if that fails.
      auto lIntRes = ToIntegral(runtime, runtime.makeHandle(*lPrimRes));
      if (LLVM_UNLIKELY(lIntRes == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      auto rIntRes = toUInt32_RJS(runtime, rhs);
      if (LLVM_UNLIKELY(rIntRes == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      auto lnum = hermes::truncateToInt32(lIntRes->getNumber());
      auto rnum = static_cast<uint32_t>(rIntRes->getNumber()) & 0x1f;
      return HermesValue::encodeTrustedNumberValue((*Oper)(lnum, rnum));
    }
    // LHS is a BigInt, try to convert RHS to a BigInt as well and perform a
    // BigInt operation.
    return doBigIntBinOp(
        runtime,
        BigIntOper,
        runtime.makeHandle(lPrimRes->getBigInt()),
        std::move(rhs));
  }();
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *res;
}
} // namespace

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_sub_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  return binOpImpl<doSub, BigIntPrimitive::subtract>(shr, a, b);
}
LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_mul_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  return binOpImpl<doMul, BigIntPrimitive::multiply>(shr, a, b);
}
LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_div_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  return binOpImpl<doDiv, BigIntPrimitive::divide>(shr, a, b);
}
LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_mod_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  return binOpImpl<doMod, BigIntPrimitive::remainder>(shr, a, b);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_inc_rjs(
    SHRuntime *shr,
    const SHLegacyValue *n) {
  return incDecOperImpl<doInc, BigIntPrimitive::inc>(shr, n);
}
LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_dec_rjs(
    SHRuntime *shr,
    const SHLegacyValue *n) {
  return incDecOperImpl<doDec, BigIntPrimitive::dec>(shr, n);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_bit_and_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  return bitOperImpl<doBitAnd, BigIntPrimitive::bitwiseAND>(shr, a, b);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_bit_or_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  return bitOperImpl<doBitOr, BigIntPrimitive::bitwiseOR>(shr, a, b);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_bit_xor_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  return bitOperImpl<doBitXor, BigIntPrimitive::bitwiseXOR>(shr, a, b);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_imul_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  Handle<> lhs{toPHV(a)}, rhs{toPHV(b)};
  // Fast path, both arguments are numbers.
  if (LLVM_LIKELY(_sh_ljs_are_both_non_nan_numbers(*a, *b)))
    return HermesValue::encodeTrustedNumberValue(doImul(
        hermes::truncateToInt32(lhs->getNumber()),
        hermes::truncateToInt32(rhs->getNumber())));

  // Math.imul(a, b) always converts both operands with ToInt32 (which
  // raises a TypeError for a BigInt operand) and never falls back to a
  // BigInt result, unlike the other bitwise binops. So, unlike
  // bitOperImpl, there is no BigInt path here.
  auto res = [&]() -> CallResult<HermesValue> {
    Runtime &runtime = getRuntime(shr);
    GCScopeMarkerRAII marker{runtime};
    auto lIntRes = toInt32_RJS(runtime, lhs);
    if (LLVM_UNLIKELY(lIntRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    int32_t lnum = lIntRes->getNumberAs<int32_t>();
    auto rIntRes = toInt32_RJS(runtime, rhs);
    if (LLVM_UNLIKELY(rIntRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    int32_t rnum = rIntRes->getNumberAs<int32_t>();
    return HermesValue::encodeTrustedNumberValue(doImul(lnum, rnum));
  }();
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *res;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_left_shift_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  return shiftOperImpl<doLShift, toUInt32_RJS, BigIntPrimitive::leftShift>(
      shr, a, b);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_unsigned_right_shift_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  return shiftOperImpl<
      doURShift,
      toUInt32_RJS,
      BigIntPrimitive::unsignedRightShift>(shr, a, b);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_right_shift_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  return shiftOperImpl<
      doRShift,
      toInt32_RJS,
      BigIntPrimitive::signedRightShift>(shr, a, b);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_bit_not_rjs(
    SHRuntime *shr,
    const SHLegacyValue *n) {
  Handle<> nHandle{toPHV(n)};
  // Fast path, nHandle is a number.
  if (nHandle->isNumber())
    return HermesValue::encodeTrustedNumberValue(
        ~hermes::truncateToInt32(nHandle->getNumber()));
  Runtime &runtime = getRuntime(shr);
  auto res = [&]() -> CallResult<HermesValue> {
    // Try converting nHandle to a numeric.
    auto numRes = toNumeric_RJS(runtime, nHandle);
    if (LLVM_UNLIKELY(numRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    // Test for BigInt since it is cheaper than testing for number. If it is a
    // number, truncate it and perform bitwise not.
    if (LLVM_LIKELY(!numRes->isBigInt()))
      return HermesValue::encodeTrustedNumberValue(
          ~hermes::truncateToInt32(numRes->getNumber()));

    // The result is a BigInt, perform a BigInt bitwise not.
    auto bigint = runtime.makeHandle(numRes->getBigInt());
    return BigIntPrimitive::unaryNOT(runtime, bigint);
  }();
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *res;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_minus_rjs(
    SHRuntime *shr,
    const SHLegacyValue *n) {
  // Fast path, n is a number.
  if (LLVM_LIKELY(toPHV(n)->isNumber()))
    return HermesValue::encodeTrustedNumberValue(-toPHV(n)->getNumber());
  auto res = [shr, n]() -> CallResult<HermesValue> {
    Runtime &runtime = getRuntime(shr);
    GCScopeMarkerRAII marker{runtime};
    Handle<> nHandle{toPHV(n)};
    // Try converting nHandle to a numeric.
    auto numRes = toNumeric_RJS(runtime, nHandle);
    if (LLVM_UNLIKELY(numRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    // Test for BigInt since it is cheaper than testing for number. If it is a
    // number, negate it.
    if (LLVM_LIKELY(!numRes->isBigInt()))
      return HermesValue::encodeTrustedNumberValue(-numRes->getNumber());

    // The result is a BigInt, perform a BigInt unary minus.
    auto bigint = runtime.makeHandle(numRes->getBigInt());
    return BigIntPrimitive::unaryMinus(runtime, bigint);
  }();
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *res;
}

extern "C" bool _sh_ljs_to_boolean(SHLegacyValue b) {
  return toBoolean(HermesValue::fromRaw(b.raw));
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" bool _sh_ljs_equal_rjs(
    SHRuntime *shr,
    const SHLegacyValue *a,
    const SHLegacyValue *b) {
  Handle<> aHandle{toPHV(a)}, bHandle{toPHV(b)};
  CallResult<bool> eqRes{false};
  {
    GCScopeMarkerRAII marker(getRuntime(shr));
    eqRes = abstractEqualityTest_RJS(getRuntime(shr), aHandle, bHandle);
  }
  if (LLVM_UNLIKELY(eqRes == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *eqRes;
}

extern "C" bool _sh_ljs_strict_equal(SHLegacyValue a, SHLegacyValue b) {
  static_assert(
      HVTag_BigInt == HVTag_Str + 1,
      "_sh_ljs_strict_equal_inline range-checks Str..BigInt");
  return strictEqualityTest(
      HermesValue::fromRaw(a.raw), HermesValue::fromRaw(b.raw));
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_typeof(SHRuntime *shr, SHLegacyValue *v) {
  return typeOf(getRuntime(shr), Handle<>::vmcast(toPHV(v)));
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" bool _sh_ljs_typeof_is(SHLegacyValue val, uint16_t typesRaw) {
  hermes::TypeOfIsTypes types(typesRaw);
  HermesValue hv = HermesValue::fromRaw(val.raw);
  return matchTypeOfIs(hv, types);
}

extern "C" SHLegacyValue _sh_ljs_add_empty_string_rjs(
    SHRuntime *shr,
    const SHLegacyValue *val) {
  Runtime &runtime = getRuntime(shr);
  if (LLVM_LIKELY(toPHV(val)->isString())) {
    return *val;
  } else {
    // Slow path, convert to primitive then call toString.
    auto res = [&runtime, val]() -> CallResult<HermesValue> {
      GCScopeMarkerRAII marker{runtime};
      auto res =
          toPrimitive_RJS(runtime, Handle<>(toPHV(val)), PreferredType::NONE);
      if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      Handle<> tmpHandle = runtime.makeHandle(*res);
      auto strRes = toString_RJS(runtime, tmpHandle);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;

      return strRes->getHermesValue();
    }();
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
      _sh_throw_current(shr);
    return *res;
  }
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_instance_of_rjs(
    SHRuntime *shr,
    SHLegacyValue *object,
    SHLegacyValue *constructor) {
  Runtime &runtime = getRuntime(shr);
  CallResult<bool> cr{false};
  {
    GCScopeMarkerRAII marker{runtime};
    cr = instanceOfOperator_RJS(
        runtime, Handle<>(toPHV(object)), Handle<>(toPHV(constructor)));
  }
  if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return _sh_ljs_bool(*cr);
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_iterator_begin_rjs(
    SHRuntime *shr,
    SHLegacyValue *src) {
  auto res = [&]() -> CallResult<HermesValue> {
    Runtime &runtime = getRuntime(shr);
    GCScopeMarkerRAII marker{runtime};
    Handle srcHandle{toPHV(src)};
    if (LLVM_LIKELY(vmisa<JSArray>(*srcHandle))) {
      // Check if we can take the fast path for array iteration, if the
      // Symbol.iterator field is unmodified.
      NamedPropertyDescriptor desc;
      JSObject *propObj = JSObject::getNamedDescriptorPredefined(
          Handle<JSArray>::vmcast(srcHandle),
          runtime,
          Predefined::SymbolIterator,
          desc);
      if (LLVM_LIKELY(propObj)) {
        auto slotValueRes = JSObject::getNamedSlotValue(
            createPseudoHandle(propObj), runtime, desc);
        if (LLVM_UNLIKELY(slotValueRes == ExecutionStatus::EXCEPTION))
          return ExecutionStatus::EXCEPTION;
        PseudoHandle<> slotValue = std::move(*slotValueRes);
        if (LLVM_LIKELY(
                slotValue->getRaw() ==
                runtime.arrayPrototypeValues.getHermesValue().getRaw()))
          return HermesValue::encodeTrustedNumberValue(0);
      }
    }

    // Fall back to the general case.
    CallResult<UncheckedIteratorRecord> iterRecord =
        getIterator(runtime, srcHandle);
    if (LLVM_UNLIKELY(iterRecord == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    *src = iterRecord->nextMethod.getHermesValue();
    return iterRecord->iterator.getHermesValue();
  }();
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *res;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" SHLegacyValue _sh_ljs_iterator_next_rjs(
    SHRuntime *shr,
    SHLegacyValue *iteratorOrIdx,
    const SHLegacyValue *srcOrNext) {
  auto res = [&]() -> CallResult<HermesValue> {
    Runtime &runtime = getRuntime(shr);
    Handle<> iteratorOrIdxHandle{toPHV(iteratorOrIdx)},
        srcOrNextHandle{toPHV(srcOrNext)};

    if (LLVM_LIKELY(iteratorOrIdxHandle->isNumber())) {
      // If the iterator is a number, we can take the fast path for array
      // iteration.
      JSArray::size_type i =
          iteratorOrIdxHandle->getNumberAs<JSArray::size_type>();
      auto arr = Handle<JSArray>::vmcast(srcOrNextHandle);

      {
        NoHandleScope noHandles{runtime};
        NoAllocScope noAlloc{runtime};

        if (i >=
            JSArray::getLength(vmcast<JSArray>(*srcOrNextHandle), runtime)) {
          // Finished iterating the array, stop.
          *iteratorOrIdx = HermesValue::encodeUndefinedValue();
          return HermesValue::encodeUndefinedValue();
        }
        // Fast path: look up the property in indexed storage.
        // Runs when there is no hole and a regular non-accessor property exists
        // at the current index, because those are the only properties stored
        // in indexed storage.
        // If there is another kind of property we have to call getComputed_RJS.
        // No need to check the fastIndexProperties flag because the indexed
        // storage would be deleted and at() would return empty in that case.
        SmallHermesValue value = arr->at(runtime, i);
        if (LLVM_LIKELY(!value.isEmpty())) {
          *iteratorOrIdx = HermesValue::encodeTrustedNumberValue(i + 1);
          return value.unboxToHV(runtime);
        }
      }
      // Slow path, just run the full getComputedPropertyValue_RJS path.
      GCScopeMarkerRAII marker{runtime};
      CallResult<PseudoHandle<>> valueRes =
          JSObject::getComputed_RJS(arr, runtime, iteratorOrIdxHandle);
      if (LLVM_UNLIKELY(valueRes == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      *iteratorOrIdx = HermesValue::encodeTrustedNumberValue(i + 1);
      return valueRes->get();
    }
    if (LLVM_UNLIKELY(iteratorOrIdxHandle->isUndefined())) {
      return HermesValue::encodeUndefinedValue();
    }

    GCScopeMarkerRAII marker{runtime};

    CheckedIteratorRecord iterRecord{
        Handle<JSObject>::vmcast(iteratorOrIdxHandle),
        Handle<Callable>::vmcast(srcOrNextHandle)};

    CallResult<PseudoHandle<JSObject>> resultObjRes =
        iteratorNext(runtime, iterRecord, llvh::None);
    if (LLVM_UNLIKELY(resultObjRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;

    Handle<JSObject> resultObj = runtime.makeHandle(std::move(*resultObjRes));
    CallResult<PseudoHandle<>> doneRes = JSObject::getNamed_RJS(
        resultObj, runtime, Predefined::getSymbolID(Predefined::done));
    if (LLVM_UNLIKELY(doneRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    if (toBoolean(doneRes->get())) {
      // Done with iteration. Clear the iterator so that subsequent
      // instructions do not call next() or return().
      *iteratorOrIdx = HermesValue::encodeUndefinedValue();
      return HermesValue::encodeUndefinedValue();
    }

    // Not done iterating, so get the `value` property and return it.
    CallResult<PseudoHandle<>> propRes = JSObject::getNamed_RJS(
        resultObj, runtime, Predefined::getSymbolID(Predefined::value));
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;

    return propRes->getHermesValue();
  }();
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    _sh_throw_current(shr);
  return *res;
}

LLVM_ATTRIBUTE_NOINLINE
extern "C" void _sh_ljs_iterator_close_rjs(
    SHRuntime *shr,
    const SHLegacyValue *iteratorOrIdx,
    bool ignoreExceptions) {
  Handle<> iteratorOrIdxHandle{toPHV(iteratorOrIdx)};
  if (LLVM_LIKELY(!iteratorOrIdxHandle->isObject()))
    return;

  Runtime &runtime = getRuntime(shr);
  ExecutionStatus res;
  {
    GCScopeMarkerRAII marker{runtime};
    // The iterator must be closed if it's still an object.
    // That means it was never an index and is not done iterating (a state
    // which is indicated by `undefined`).
    res = iteratorClose(
        runtime,
        Handle<JSObject>::vmcast(iteratorOrIdxHandle),
        Runtime::getEmptyValue());
  }

  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    // Ignore the raised exception if necessary.
    if (ignoreExceptions && !isUncatchableError(*runtime.thrownValue_)) {
      runtime.clearThrownValue();
      return;
    }

    _sh_throw_current(shr);
  }
}

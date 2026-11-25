/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES5.1 15.5 Initialize the String constructor.
//===----------------------------------------------------------------------===//
#include "JSLibInternal.h"

#include "hermes/Platform/Unicode/PlatformUnicode.h"
#include "hermes/VM/CallResult.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/PrimitiveBox.h"
#include "hermes/VM/SmallXString.h"
#include "hermes/VM/StringBuilder.h"
#include "hermes/VM/StringView.h"

#if defined(__ANDROID__)
#include "hermes/Platform/Unicode/PlatformUnicode.h"
#endif

#ifdef __APPLE__
#include <CoreFoundation/CFString.h>
#endif

#include <locale>

namespace hermes {
namespace vm {

//===----------------------------------------------------------------------===//
/// String.

HermesValue createStringConstructor(Runtime &runtime) {
  struct : public Locals {
    PinnedValue<NativeFunction> trimStart;
    PinnedValue<NativeFunction> trimEnd;
    PinnedValue<NativeConstructor> cons;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  Handle<JSString> stringPrototype{runtime.stringPrototype};

  defineSystemConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::String),
      stringConstructor,
      stringPrototype,
      1,
      lv.cons);

  // String.prototype.xxx methods.
  void *ctx = nullptr;
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::toString),
      ctx,
      stringPrototypeToString,
      0);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::at),
      ctx,
      stringPrototypeAt,
      1);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::valueOf),
      ctx,
      stringPrototypeToString,
      0);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::charCodeAt),
      ctx,
      stringPrototypeCharCodeAt,
      1);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::codePointAt),
      ctx,
      stringPrototypeCodePointAt,
      1);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::concat),
      ctx,
      stringPrototypeConcat,
      1);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::substring),
      ctx,
      stringPrototypeSubstring,
      2);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::toLowerCase),
      ctx,
      stringPrototypeToLowerCase,
      0);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::toLocaleLowerCase),
      ctx,
      stringPrototypeToLocaleLowerCase,
      0);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::toUpperCase),
      ctx,
      stringPrototypeToUpperCase,
      0);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::toLocaleUpperCase),
      ctx,
      stringPrototypeToLocaleUpperCase,
      0);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::substr),
      ctx,
      stringPrototypeSubstr,
      2);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::trim),
      ctx,
      stringPrototypeTrim,
      0);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::localeCompare),
      ctx,
      stringPrototypeLocaleCompare,
      1);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::normalize),
      ctx,
      stringPrototypeNormalize,
      0);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::repeat),
      ctx,
      stringPrototypeRepeat,
      1);

  DefinePropertyFlags dpf = DefinePropertyFlags::getNewNonEnumerableFlags();
  lv.trimStart = defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::trimStart),
      Predefined::getSymbolID(Predefined::trimStart),
      ctx,
      stringPrototypeTrimStart,
      0,
      dpf);
  lv.trimEnd = defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::trimEnd),
      Predefined::getSymbolID(Predefined::trimEnd),
      ctx,
      stringPrototypeTrimEnd,
      0,
      dpf);

  defineProperty(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::trimLeft),
      lv.trimStart);
  defineProperty(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::trimRight),
      lv.trimEnd);

  (void)defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::SymbolIterator),
      Predefined::getSymbolID(Predefined::squareSymbolIterator),
      ctx,
      stringPrototypeSymbolIterator,
      0,
      dpf);

  // String.xxx() methods.
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::fromCharCode),
      ctx,
      stringFromCharCode,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::fromCodePoint),
      ctx,
      stringFromCodePoint,
      1);
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::raw),
      ctx,
      stringRaw,
      1);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::matchAll),
      ctx,
      stringPrototypeMatchAll,
      1);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::replaceAll),
      ctx,
      stringPrototypeReplaceAll,
      2);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::match),
      ctx,
      stringPrototypeMatch,
      1);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::padEnd),
      (void *)false,
      stringPrototypePad,
      1);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::padStart),
      (void *)true,
      stringPrototypePad,
      1);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::replace),
      ctx,
      stringPrototypeReplace,
      2);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::search),
      ctx,
      stringPrototypeSearch,
      1);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::charAt),
      ctx,
      stringPrototypeCharAt,
      1);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::endsWith),
      ctx,
      stringPrototypeEndsWith,
      1);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::slice),
      ctx,
      stringPrototypeSlice,
      2);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::split),
      ctx,
      stringPrototypeSplit,
      2);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::includes),
      (void *)false,
      stringPrototypeIncludesOrStartsWith,
      1);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::indexOf),
      ctx,
      stringPrototypeIndexOf,
      1);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::lastIndexOf),
      ctx,
      stringPrototypeLastIndexOf,
      1);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::startsWith),
      (void *)true,
      stringPrototypeIncludesOrStartsWith,
      1);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::isWellFormed),
      ctx,
      stringPrototypeIsWellFormed,
      0);
  defineMethod(
      runtime,
      stringPrototype,
      Predefined::getSymbolID(Predefined::toWellFormed),
      ctx,
      stringPrototypeToWellFormed,
      0);

  return lv.cons.getHermesValue();
}

// ES2024 22.1.1.1 String(value)
CallResult<HermesValue> stringConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  PseudoHandle<StringPrimitive> strPrim;
  // 1. If value is not present, then
  if (args.getArgCount() == 0) {
    // a. Let s be the empty String.
    strPrim = createPseudoHandle(
        runtime.getPredefinedString(Predefined::emptyString));
  } else {
    // 2. Else,
    // a. If NewTarget is undefined and value is a Symbol, return
    // SymbolDescriptiveString(value).
    if (!args.isConstructorCall() && args.getArg(0).isSymbol()) {
      auto strPrimRes = symbolDescriptiveString(
          runtime, Handle<SymbolID>::vmcast(args.getArgHandle(0)));
      if (LLVM_UNLIKELY(strPrimRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      return strPrimRes->getHermesValue();
    }
    // b. Let s be ? ToString(value).
    auto strPrimRes = toString_RJS(runtime, args.getArgHandle(0));
    if (strPrimRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    strPrim = std::move(*strPrimRes);
  }

  // 3. If NewTarget is undefined, return s.
  if (!args.isConstructorCall()) {
    return strPrim.getHermesValue();
  }

  // 4. Return StringCreate(s, ? GetPrototypeFromConstructor(NewTarget,
  // "%String.prototype%")).
  struct : public Locals {
    PinnedValue<StringPrimitive> s;
    PinnedValue<JSObject> selfParent;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.s = std::move(strPrim);

  if (LLVM_LIKELY(
          args.getNewTarget().getRaw() ==
          runtime.stringConstructor.getHermesValue().getRaw())) {
    auto selfRes = JSString::create(runtime, lv.s, runtime.stringPrototype);
    if (LLVM_UNLIKELY(selfRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return selfRes->getHermesValue();
  }
  CallResult<PseudoHandle<JSObject>> thisParentRes =
      NativeConstructor::parentForNewThis_RJS(
          runtime,
          Handle<Callable>::vmcast(&args.getNewTarget()),
          runtime.stringPrototype);
  if (LLVM_UNLIKELY(thisParentRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.selfParent = std::move(*thisParentRes);
  auto selfRes = JSString::create(runtime, lv.s, lv.selfParent);
  if (LLVM_UNLIKELY(selfRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return selfRes->getHermesValue();
}

CallResult<HermesValue> stringFromCharCode(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope(runtime);
  uint32_t n = args.getArgCount();
  if (LLVM_LIKELY(n == 1)) {
    // Fast path for when only one argument is provided.
    auto res = toUInt16(runtime, args.getArgHandle(0));
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    char16_t ch = res->getNumber();
    return runtime.getCharacterString(ch).getHermesValue();
  }
  // Convert every argument first, so the result is allocated once with its
  // encoding known.
  llvh::SmallVector<char16_t, 32> codeUnits;
  codeUnits.reserve(n);
  bool allASCII = true;
  for (unsigned i = 0; i < n; ++i) {
    // Call a function that may throw, let the runtime record it.
    auto res = toUInt16(runtime, args.getArgHandle(i));
    if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    char16_t ch = res->getNumber();
    allASCII &= ch < 128;
    codeUnits.push_back(ch);
  }
  struct : public Locals {
    PinnedValue<StringPrimitive> result;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  auto builder = StringBuilder::createStringBuilder(
      runtime, lv.result, SafeUInt32{n}, allASCII);
  if (builder == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  for (char16_t ch : codeUnits)
    builder->appendCharacter(ch);
  return HermesValue::encodeStringValue(*builder->getStringPrimitive());
}

CallResult<HermesValue> stringFromCodePoint(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};

  // 1. Let codePoints be a List containing the arguments passed to this
  // function.
  // 2. Let length be the number of elements in codePoints.
  uint32_t length = args.getArgCount();
  // 3. Let elements be a new List.
  llvh::SmallVector<char16_t, 32> elements{};
  // 4. Let nextIndex be 0.
  uint32_t nextIndex = 0;

  struct : public Locals {
    PinnedValue<> next;
    PinnedValue<> nextCP;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  GCScopeMarkerRAII marker{gcScope};
  // 5. Repeat while nextIndex < length
  for (; nextIndex < length; marker.flush()) {
    marker.flush();
    // 5a. Let next be codePoints[nextIndex].
    lv.next = args.getArg(nextIndex);
    // 5b. Let nextCP be toNumber_RJS(next).
    auto nextCPRes = toNumber_RJS(runtime, lv.next);
    if (LLVM_UNLIKELY(nextCPRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.nextCP = *nextCPRes;

    // 5d. If SameValue(nextCP, ToIntegerOrInfinity(nextCP)) is false, throw
    // a RangeError exception.
    auto nextCPInt = toIntegerOrInfinity(runtime, lv.nextCP);
    if (LLVM_UNLIKELY(nextCPInt == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (!isSameValue(lv.nextCP.get(), *nextCPInt)) {
      return runtime.raiseRangeError(
          TwineChar16("Code point must be an integer: ") +
          lv.nextCP->getNumber());
    }

    // 5e. If nextCP < 0 or nextCP > 0x10FFFF, throw a RangeError exception.
    if (lv.nextCP->getNumber() < 0 || lv.nextCP->getNumber() > 0x10FFFF) {
      return runtime.raiseRangeError(
          TwineChar16("Code point out of bounds: ") + lv.nextCP->getNumber());
    }

    // 5f. Append the elements of the UTF16Encoding (10.1.1) of nextCP to the
    // end of elements.
    // Safe to get as uint32_t because we've done int and bounds checking.
    utf16Encoding(lv.nextCP->getNumberAs<uint32_t>(), elements);

    // 5g. Let nextIndex be nextIndex + 1.
    ++nextIndex;
  }

  // 6. Return the String value whose elements are, in order, the elements in
  // the List elements. If length is 0, the empty string is returned.
  return StringPrimitive::createEfficient(runtime, elements);
}

/// ES6.0 21.1.2.4 String.raw ( template , ...substitutions )
CallResult<HermesValue> stringRaw(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};

  struct : public Locals {
    PinnedValue<JSObject> cooked;
    PinnedValue<> getRes;
    PinnedValue<JSObject> raw;
    PinnedValue<> lengthRes;
    PinnedValue<> nextIndex;
    PinnedValue<> tmpHandle;
    PinnedValue<StringPrimitive> nextSeg;
    PinnedValue<> next;
    PinnedValue<StringPrimitive> nextSub;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // 1. Let substitutions be a List consisting of all of the arguments passed to
  // this function, starting with the second argument.
  // If fewer than two arguments were passed, the List is empty.
  // 2. Let numberOfSubstitutions be the number of elements in substitutions.
  uint32_t numberOfSubstitutions =
      args.getArgCount() < 2 ? 0 : args.getArgCount() - 1;

  // 3. Let cooked be ToObject(template).
  auto cookedRes = toObject(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(cookedRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.cooked = vmcast<JSObject>(*cookedRes);

  // 5. Let raw be ToObject(Get(cooked, "raw")).
  auto getRes = JSObject::getNamed_RJS(
      lv.cooked, runtime, Predefined::getSymbolID(Predefined::raw));
  if (LLVM_UNLIKELY(getRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.getRes = std::move(*getRes);
  auto rawRes = toObject(runtime, Handle<>{lv.getRes});
  if (LLVM_UNLIKELY(rawRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.raw = vmcast<JSObject>(*rawRes);

  // 7. Let literalSegments be ToLength(Get(raw, "length"))
  auto lengthRes = JSObject::getNamed_RJS(
      lv.raw, runtime, Predefined::getSymbolID(Predefined::length));
  if (LLVM_UNLIKELY(lengthRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.lengthRes = std::move(*lengthRes);
  auto literalSegmentsRes = toLength(runtime, Handle<>{lv.lengthRes});
  if (LLVM_UNLIKELY(literalSegmentsRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  int64_t literalSegments = literalSegmentsRes->getNumberAs<int64_t>();
  // 9. If literalSegments ≤ 0, return the empty string.
  if (literalSegments <= 0) {
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::emptyString));
  }

  // 10. Let stringElements be a new List.
  llvh::SmallVector<char16_t, 32> stringElements{};

  // 11. Let nextIndex be 0.
  lv.nextIndex = HermesValue::encodeTrustedNumberValue(0);

  // 12. Repeat
  GCScopeMarkerRAII marker{gcScope};
  for (;; marker.flush()) {
    // 12. a. Let nextKey be ToString(nextIndex).
    // 12. b. Let nextSeg be ToString(Get(raw, nextKey)).
    auto nextSegPropRes =
        JSObject::getComputed_RJS(lv.raw, runtime, lv.nextIndex);
    if (LLVM_UNLIKELY(nextSegPropRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.tmpHandle = std::move(*nextSegPropRes);
    auto nextSegRes = toString_RJS(runtime, lv.tmpHandle);
    if (LLVM_UNLIKELY(nextSegRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.nextSeg = nextSegRes->get();

    // 12. d. Append in order the code unit elements of nextSeg to the end of
    // stringElements.
    lv.nextSeg->appendUTF16String(stringElements);

    // 12. e. If nextIndex + 1 = literalSegments, then
    if (lv.nextIndex->getNumberAs<int64_t>() + 1 == literalSegments) {
      // 12. i. Return the String value whose code units are, in order, the
      // elements in the List stringElements. If stringElements has no elements,
      // the empty string is returned.
      return StringPrimitive::createEfficient(runtime, stringElements);
    }

    if (lv.nextIndex->getNumberAs<int64_t>() < numberOfSubstitutions) {
      // 12. f. If nextIndex < numberOfSubstitutions, let next be
      // substitutions[nextIndex].
      // Add one to nextIndex to get index in substitutions.
      lv.next = args.getArg(lv.nextIndex->getNumberAs<int64_t>() + 1);
      // 12. h. Let nextSub be ToString(next).
      auto nextSubRes = toString_RJS(runtime, lv.next);
      if (LLVM_UNLIKELY(nextSubRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.nextSub = nextSubRes->get();
      // 12. j. Append in order the code unit elements of nextSub to the end of
      // stringElements.
      lv.nextSub->appendUTF16String(stringElements);
    }

    // 12. g. Else, let next be the empty String.
    // Omitted because nothing happens.

    // 12. k. Let nextIndex be nextIndex + 1.
    lv.nextIndex = HermesValue::encodeTrustedNumberValue(
        lv.nextIndex->getNumberAs<int64_t>() + 1);
  }
}

//===----------------------------------------------------------------------===//
/// String.prototype.

/// 22.1.3.1
CallResult<HermesValue> stringPrototypeAt(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope(runtime);

  struct : public Locals {
    PinnedValue<StringPrimitive> S;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  // 1. Let O be RequireObjectCoercible(this value).
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, args.getThisHandle()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 2. Let S be ToString(O).
  auto strRes = toString_RJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);

  // 3. Let len be the length of S.
  double len = lv.S->getStringLength();

  // 4. Let relativeIndex be ? ToIntegerOrInfinity(index).
  auto idx = args.getArgHandle(0);
  auto relativeIndexRes = toIntegerOrInfinity(runtime, idx);
  if (LLVM_UNLIKELY(relativeIndexRes == ExecutionStatus::EXCEPTION)) {
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

  // 6. If k < 0 or k ≥ len, return undefined.
  if (k < 0 || k >= len) {
    return HermesValue::encodeUndefinedValue();
  }

  // 8. Return the substring of S from k to k + 1.
  auto sliceRes = StringPrimitive::slice(runtime, lv.S, k, 1);
  if (LLVM_UNLIKELY(sliceRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return sliceRes;
}

CallResult<HermesValue> stringPrototypeToString(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  if (args.getThisArg().isString()) {
    return args.getThisArg();
  }

  // Not a String value, must be a string object.
  auto *strPtr = dyn_vmcast<JSString>(args.getThisArg());
  if (strPtr) {
    // Only return the string if called on a String object.
    return HermesValue::encodeStringValue(
        JSString::getPrimitiveString(strPtr, runtime));
  }
  return runtime.raiseTypeError(
      "String.prototype.toString() called on non-string object");
}

CallResult<HermesValue> stringPrototypeCharCodeAt(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  Handle<> thisValue{&args.getThisArg()};

  struct : public Locals {
    PinnedValue<StringPrimitive> S;
    PinnedValue<> argValue;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  // Call a function that may throw, let the runtime record it.
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, thisValue) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto strRes = toString_RJS(runtime, thisValue);
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);
  lv.argValue = args.getArg(0);
  auto intRes = toIntegerOrInfinity(runtime, lv.argValue);
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto position = intRes->getNumber();
  auto size = lv.S->getStringLength();
  if (position < 0 || position >= size) {
    return HermesValue::encodeNaNValue();
  }
  return HermesValue::encodeTrustedNumberValue(
      StringPrimitive::createStringView(runtime, lv.S)[position]);
}

CallResult<HermesValue> stringPrototypeCodePointAt(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<StringPrimitive> S;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 1. Let O be RequireObjectCoercible(this value).
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, args.getThisHandle()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 2. Let S be ToString(O).
  auto strRes = toString_RJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);

  // 4. Let position be ToIntegerOrInfinity(pos).
  auto positionRes = toIntegerOrInfinity(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(positionRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double position = positionRes->getNumber();

  // 6. Let size be the number of elements in S.
  double size = lv.S->getStringLength();

  // 7. If position < 0 or position ≥ size, return undefined.
  if (position < 0 || position >= size) {
    return HermesValue::encodeUndefinedValue();
  }

  auto strView = StringPrimitive::createStringView(runtime, lv.S);

  // 8. Let first be the code unit value of the element at index position in the
  // String S.
  char16_t first = strView[position];

  // 9. If first < 0xD800 or first > 0xDBFF or position+1 = size, return first.
  if (first < 0xD800 || first > 0xDBFF || position + 1 == size) {
    return HermesValue::encodeTrustedNumberValue(first);
  }

  // 10. Let second be the code unit value of the element at index position+1 in
  // the String S.
  // Safe to access because we ensured that position + 1 < size.
  char16_t second = strView[position + 1];

  // 11. If second < 0xDC00 or second > 0xDFFF, return first.
  if (second < 0xDC00 || second > 0xDFFF) {
    return HermesValue::encodeTrustedNumberValue(first);
  }

  // 12. Return UTF16Decode(first, second).
  return HermesValue::encodeTrustedNumberValue(utf16Decode(first, second));
}

CallResult<HermesValue> stringPrototypeConcat(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope(runtime);

  struct : public Locals {
    PinnedValue<StringPrimitive> S;
    PinnedValue<ArrayStorageSmall> strings;
    PinnedValue<StringPrimitive> element;
    PinnedValue<StringPrimitive> result;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, args.getThisHandle()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto strRes = toString_RJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);
  // Track the total characters in the result.
  SafeUInt32 size(lv.S->getStringLength());
  uint32_t argCount = args.getArgCount();

  // Store the results of toStrings and concat them at the end.
  auto arrRes = ArrayStorageSmall::create(runtime, argCount, argCount);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.strings = vmcast<ArrayStorageSmall>(*arrRes);

  // Track if all strings are ASCII.
  bool allASCII = lv.S->isASCII();
  // Run toString on the arguments to figure out the final size.
  auto marker = gcScope.createMarker();
  for (uint32_t i = 0; i < argCount; ++i) {
    auto strRes = toString_RJS(runtime, args.getArgHandle(i));
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // Allocations can't be performed here,
    // and we know we're in bounds because we preallocated.
    lv.strings->set(
        i,
        SmallHermesValue::encodeStringValue(strRes->get(), runtime),
        runtime.getHeap());
    uint32_t strLength = strRes->get()->getStringLength();

    allASCII &= (*strRes)->isASCII();
    size.add(strLength);
    if (LLVM_UNLIKELY(size.isOverflowed())) {
      return runtime.raiseRangeError("resulting string length exceeds limit");
    }

    gcScope.flushToMarker(marker);
  }

  // Allocate the complete result.
  auto builder =
      StringBuilder::createStringBuilder(runtime, lv.result, size, allASCII);
  if (builder == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  // Copy 'this' argument first.
  builder->appendStringPrim(lv.S);

  // Copy the rest of the strings.
  for (uint32_t i = 0; i < argCount; i++) {
    lv.element = lv.strings->at(i).getString(runtime);
    builder->appendStringPrim(lv.element);
  }
  return builder->getStringPrimitive().getHermesValue();
}

CallResult<HermesValue> stringPrototypeSubstring(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<StringPrimitive> S;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, args.getThisHandle()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto strRes = toString_RJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);
  double len = lv.S->getStringLength();

  auto intRes = toIntegerOrInfinity(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double intStart = intRes->getNumber();

  double intEnd;
  if (args.getArg(1).isUndefined()) {
    intEnd = len;
  } else {
    if (LLVM_UNLIKELY(
            (intRes = toIntegerOrInfinity(runtime, args.getArgHandle(1))) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    intEnd = intRes->getNumber();
  }

  size_t finalStart = std::min(std::max(intStart, 0.0), len);
  size_t finalEnd = std::min(std::max(intEnd, 0.0), len);
  size_t from = std::min(finalStart, finalEnd);
  size_t to = std::max(finalStart, finalEnd);

  return StringPrimitive::slice(runtime, lv.S, from, to > from ? to - from : 0);
}

static CallResult<HermesValue> convertCase(
    Runtime &runtime,
    Handle<StringPrimitive> S,
    const bool upperCase,
    const bool useCurrentLocale) {
  // Copying is unavoidable in this function, do it early on.
  SmallU16String<32> buff;
  // Must copy instead of just getting the reference, because later operations
  // may trigger GC and hence invalid pointers inside S.
  S->appendUTF16String(buff);
  UTF16Ref str = buff.arrayRef();

  {
    // Try a fast path for ASCII strings.
    // First, bitwise-or all the characters to see if any one isn't ASCII.
    char16_t mask = 0;
    // Also, check if we have to do work or we can just return S directly.
    bool noop = true;
    if (upperCase) {
      for (const auto c : str) {
        mask |= c;
        // It's still a noop if the character isn't a lowercase ASCII.
        noop &= !('a' <= c && c <= 'z');
      }
    } else {
      for (const auto c : str) {
        mask |= c;
        // It's still a noop if the character isn't an uppercase ASCII.
        noop &= !('A' <= c && c <= 'Z');
      }
    }
    // localeAffectsCasing() is only consulted once the scan says the fast
    // path is otherwise available: the ICU and Java backends query the host
    // locale on every call (a JNI round trip on Java), and a non-ASCII
    // string can never take this path, so asking first would charge every
    // locale-sensitive conversion for an answer it cannot use.
    if (mask <= 127 &&
        (!useCurrentLocale || !platform_unicode::localeAffectsCasing())) {
      if (noop) {
        // We don't have to allocate anything.
        return S.getHermesValue();
      }

      if (str.size() == 1) {
        // Use the Runtime stored representations of single-character strings.
        char16_t c = str[0];
        if (upperCase) {
          char16_t isLower = 'a' <= c && c <= 'z';
          return runtime.getCharacterString(c & ~(isLower << 5))
              .getHermesValue();
        } else {
          char16_t isUpper = 'A' <= c && c <= 'Z';
          return runtime.getCharacterString(c | (isUpper << 5))
              .getHermesValue();
        }
      }

      struct : public Locals {
        PinnedValue<StringPrimitive> result;
      } lv;
      LocalsRAII lraii(runtime, &lv);
      SafeUInt32 len(S->getStringLength());
      // Every character is ASCII (mask <= 127), and so is its case mapping.
      auto builder =
          StringBuilder::createStringBuilder(runtime, lv.result, len, true);
      if (builder == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      char16_t ch;
      if (upperCase) {
        for (const char16_t c : str) {
          // If it is lower, then clear the 5th bit, else do nothing.
          char16_t isLower = 'a' <= c && c <= 'z';
          ch = c & ~(isLower << 5);
          builder->appendCharacter(ch);
        }
      } else {
        for (const char16_t c : str) {
          // If it is upper, then set the 5th bit, else do nothing.
          char16_t isUpper = 'A' <= c && c <= 'Z';
          ch = c | (isUpper << 5);
          builder->appendCharacter(ch);
        }
      }
      return HermesValue::encodeStringValue(*builder->getStringPrimitive());
    }
  }
  platform_unicode::convertToCase(
      buff,
      upperCase ? platform_unicode::CaseConversion::ToUpper
                : platform_unicode::CaseConversion::ToLower,
      useCurrentLocale);
  return StringPrimitive::create(runtime, buff);
}

CallResult<HermesValue> stringPrototypeToLowerCase(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  struct : public Locals {
    PinnedValue<StringPrimitive> res;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, args.getThisHandle()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto resCall = toString_RJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(resCall == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.res = std::move(*resCall);
  return convertCase(runtime, lv.res, false, false);
}

CallResult<HermesValue> stringPrototypeToLocaleLowerCase(
    void *ctx,
    Runtime &runtime) {
#ifdef HERMES_ENABLE_INTL
  return intlStringPrototypeToLocaleLowerCase(/* unused */ ctx, runtime);
#else
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  struct : public Locals {
    PinnedValue<StringPrimitive> res;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, args.getThisHandle()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto resCall = toString_RJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(resCall == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.res = std::move(*resCall);
  return convertCase(runtime, lv.res, false, true);
#endif
}

CallResult<HermesValue> stringPrototypeToUpperCase(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  struct : public Locals {
    PinnedValue<StringPrimitive> res;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, args.getThisHandle()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto resCall = toString_RJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(resCall == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.res = std::move(*resCall);
  return convertCase(runtime, lv.res, true, false);
}

CallResult<HermesValue> stringPrototypeToLocaleUpperCase(
    void *ctx,
    Runtime &runtime) {
#ifdef HERMES_ENABLE_INTL
  return intlStringPrototypeToLocaleUpperCase(/* unused */ ctx, runtime);
#else
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  struct : public Locals {
    PinnedValue<StringPrimitive> res;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, args.getThisHandle()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto resCall = toString_RJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(resCall == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.res = std::move(*resCall);
  return convertCase(runtime, lv.res, true, true);
#endif
}

CallResult<HermesValue> stringPrototypeSubstr(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<StringPrimitive> S;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, args.getThisHandle()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto strRes = toString_RJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);
  double stringLen = lv.S->getStringLength();

  auto intRes = toIntegerOrInfinity(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double start = intRes->getNumber();

  double length;
  if (args.getArg(1).isUndefined()) {
    length = stringLen;
  } else {
    if (LLVM_UNLIKELY(
            (intRes = toIntegerOrInfinity(runtime, args.getArgHandle(1))) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    length = intRes->getNumber();
  }

  if (start < 0) {
    start = std::max(stringLen + start, 0.0);
  }
  double adjustedLength = std::min(std::max(length, 0.0), stringLen - start);
  if (adjustedLength <= 0) {
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::emptyString));
  } else {
    return StringPrimitive::slice(
        runtime,
        lv.S,
        static_cast<size_t>(start),
        static_cast<size_t>(adjustedLength));
  }
}

/// \return the number of characters to trim from the begin iterator.
static size_t trimStart(
    StringView::const_iterator begin,
    StringView::const_iterator end) {
  size_t toTrim = 0;
  while (begin != end &&
         (isWhiteSpaceChar(*begin) || isLineTerminatorChar(*begin))) {
    ++begin;
    ++toTrim;
  }
  return toTrim;
}

/// \return the number of characters to trim from the end iterator.
static size_t trimEnd(
    StringView::const_iterator begin,
    StringView::const_iterator end) {
  size_t toTrim = 0;
  while (begin != end &&
         (isWhiteSpaceChar(*(end - 1)) || isLineTerminatorChar(*(end - 1)))) {
    --end;
    ++toTrim;
  }
  return toTrim;
}

CallResult<HermesValue> stringPrototypeTrim(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<StringPrimitive> S;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, args.getThisHandle()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto res = toString_RJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*res);

  // Move begin and end to point to the first and last non-whitespace chars.
  size_t beginIdx = 0, endIdx = lv.S->getStringLength();
  {
    auto str = StringPrimitive::createStringView(runtime, lv.S);
    auto begin = str.begin();
    auto end = str.end();
    beginIdx = trimStart(begin, end);
    begin += beginIdx;
    endIdx -= trimEnd(begin, end);
  }

  return StringPrimitive::slice(runtime, lv.S, beginIdx, endIdx - beginIdx);
}

CallResult<HermesValue> stringPrototypeTrimStart(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<StringPrimitive> S;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, args.getThisHandle()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto res = toString_RJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*res);

  // Move begin and end to point to the first and last non-whitespace chars.
  size_t beginIdx = 0;
  {
    auto str = StringPrimitive::createStringView(runtime, lv.S);
    auto begin = str.begin();
    auto end = str.end();
    beginIdx = trimStart(begin, end);
  }

  return StringPrimitive::slice(
      runtime, lv.S, beginIdx, lv.S->getStringLength() - beginIdx);
}

CallResult<HermesValue> stringPrototypeTrimEnd(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<StringPrimitive> S;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, args.getThisHandle()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto res = toString_RJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*res);

  // Move begin and end to point to the first and last non-whitespace chars.
  size_t endIdx = lv.S->getStringLength();
  {
    auto str = StringPrimitive::createStringView(runtime, lv.S);
    auto begin = str.begin();
    auto end = str.end();
    endIdx -= trimEnd(begin, end);
  }

  return StringPrimitive::slice(runtime, lv.S, 0, endIdx);
}

CallResult<HermesValue> stringPrototypeLocaleCompare(
    void *ctx,
    Runtime &runtime) {
#ifdef HERMES_ENABLE_INTL
  return intlStringPrototypeLocaleCompare(/* unused */ ctx, runtime);
#else
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<StringPrimitive> S;
    PinnedValue<StringPrimitive> T;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto thisValue = args.getThisHandle();
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, thisValue) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto sRes = toString_RJS(runtime, thisValue);
  if (LLVM_UNLIKELY(sRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*sRes);

  auto tRes = toString_RJS(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(tRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // "That" string.
  lv.T = std::move(*tRes);

  llvh::SmallVector<char16_t, 32> left;
  llvh::SmallVector<char16_t, 32> right;

  StringPrimitive::createStringView(runtime, lv.S).appendUTF16String(left);
  StringPrimitive::createStringView(runtime, lv.T).appendUTF16String(right);
  int comparisonResult = platform_unicode::localeCompare(left, right);
  assert(comparisonResult >= -1 && comparisonResult <= 1);
  return HermesValue::encodeTrustedNumberValue(comparisonResult);
#endif
}

CallResult<HermesValue> stringPrototypeNormalize(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  using platform_unicode::NormalizationForm;
  struct : public Locals {
    PinnedValue<StringPrimitive> S;
    PinnedValue<StringPrimitive> f;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 1. Let O be RequireObjectCoercible(this value).
  auto O = args.getThisHandle();
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, O) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 2. Let S be ToString(O).
  auto sRes = toString_RJS(runtime, O);
  if (LLVM_UNLIKELY(sRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*sRes);

  NormalizationForm form;

  // 4. If form is not provided or form is undefined, let form be "NFC".
  if (args.getArg(0).isUndefined()) {
    form = NormalizationForm::C;
  } else {
    // 5. Let f be ToString(form).
    auto fRes = toString_RJS(runtime, args.getArgHandle(0));
    if (LLVM_UNLIKELY(fRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.f = std::move(*fRes);

    // 7. If f is not one of "NFC", "NFD", "NFKC", or "NFKD", throw a RangeError
    // exception.
    auto sv = StringPrimitive::createStringView(runtime, lv.f);
    if (sv.equals(ASCIIRef{"NFC", 3})) {
      form = NormalizationForm::C;
    } else if (sv.equals(ASCIIRef{"NFD", 3})) {
      form = NormalizationForm::D;
    } else if (sv.equals(ASCIIRef{"NFKC", 4})) {
      form = NormalizationForm::KC;
    } else if (sv.equals(ASCIIRef{"NFKD", 4})) {
      form = NormalizationForm::KD;
    } else {
      return runtime.raiseRangeError(
          TwineChar16("Invalid normalization form: ") + *lv.f);
    }
  }

  // 8. Let ns be the String value that is the result of normalizing S into the
  // normalization form named by f as specified in
  // http://www.unicode.org/reports/tr15/tr15-29.html.
  llvh::SmallVector<char16_t, 32> ns;
  lv.S->appendUTF16String(ns);
  platform_unicode::normalize(ns, form);

  // 9. Return ns.
  return StringPrimitive::createEfficient(runtime, ns);
}

CallResult<HermesValue> stringPrototypeRepeat(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<StringPrimitive> S;
    PinnedValue<StringPrimitive> result;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 1. Let O be RequireObjectCoercible(this value).
  auto O = args.getThisHandle();
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, O) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 2. Let S be ToString(O).
  // 3. ReturnIfAbrupt(S).
  auto sRes = toString_RJS(runtime, O);
  if (LLVM_UNLIKELY(sRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*sRes);

  // 4. Let n be ToIntegerOrInfinity(count).
  // 5. ReturnIfAbrupt(n).
  auto nRes = toIntegerOrInfinity(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(nRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double n = nRes->getNumber();

  // 6. If n < 0, throw a RangeError exception.
  // 7. If n is +Infinity, throw a RangeError exception.
  if (n < 0 || n == std::numeric_limits<double>::infinity()) {
    return runtime.raiseRangeError(
        "String.prototype.repeat count must be finite and non-negative");
  }

  // 8. Let T be a String value that is made from n copies of S appended
  // together. If n is 0, T is the empty String.
  double strLen = lv.S->getStringLength();

  if (n == 0 || strLen == 0) {
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::emptyString));
  }

  if (n > std::numeric_limits<uint32_t>::max() ||
      lv.S->getStringLength() >
          (double)StringPrimitive::MAX_STRING_LENGTH / n) {
    // Check for overflow.
    return runtime.raiseRangeError(
        "String.prototype.repeat result exceeds limit");
  }

  // It's safe to multiply as the overflow check is done above.
  SafeUInt32 finalLen(strLen * n);

  // The result is ASCII exactly when S is.
  auto builderRes = StringBuilder::createStringBuilder(
      runtime, lv.result, finalLen, lv.S->isASCII());
  if (LLVM_UNLIKELY(builderRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // Using uint32_t for i is valid because we have bounds-checked n.
  for (uint32_t i = 0; i < n; ++i) {
    builderRes->appendStringPrim(lv.S);
  }

  // 9. Return T.
  return builderRes->getStringPrimitive().getHermesValue();
}

/// ES6.0 21.1.3.27 String.prototype [ @@iterator ]( )
CallResult<HermesValue> stringPrototypeSymbolIterator(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<StringPrimitive> string;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 1. Let O be RequireObjectCoercible(this value).
  auto thisValue = args.getThisHandle();
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, thisValue) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 2. Let S be ToString(O).
  // 3. ReturnIfAbrupt(S).
  auto strRes = toString_RJS(runtime, thisValue);
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.string = std::move(*strRes);

  // 4. Return CreateStringIterator(S).
  return JSStringIterator::create(runtime, lv.string).getHermesValue();
}

CallResult<HermesValue> stringPrototypeMatchAll(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  struct : public Locals {
    PinnedValue<> flags;
    PinnedValue<StringPrimitive> flagsStr;
    PinnedValue<> matchAllSymbol;
    PinnedValue<> matcher;
    PinnedValue<StringPrimitive> S;
    PinnedValue<JSRegExp> rx;
    PinnedValue<> propValue;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  // 1. Let O be ? RequireObjectCoercible(this value).
  auto O = args.getThisHandle();
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, O) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 2. If regexp is an Object, then
  auto regexp = args.getArgHandle(0);
  if (regexp->isObject()) {
    // a. Let isRegExp be ? IsRegExp(regexp).
    auto isRegExpRes = isRegExp(runtime, regexp);
    if (LLVM_UNLIKELY(isRegExpRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // b. If isRegExp is true, then
    if (*isRegExpRes) {
      // Passing undefined and null checks imply regexp is an ObjectCoercible.
      Handle<JSObject> regexpObj = Handle<JSObject>::vmcast(regexp);
      bool isGlobal = false;
      // i. Let flags be ? Get(regexp, "flags").
      auto flagsPropRes = JSObject::getNamed_RJS(
          regexpObj, runtime, Predefined::getSymbolID(Predefined::flags));
      if (LLVM_UNLIKELY(flagsPropRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.flags = std::move(*flagsPropRes);
      // ii. Perform ? RequireObjectCoercible(flags).
      if (LLVM_UNLIKELY(
              checkObjectCoercible(runtime, lv.flags) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      // iii. If ? ToString(flags) does not contain "g", throw a TypeError
      // exception.
      auto strRes = toString_RJS(runtime, lv.flags);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.flagsStr = std::move(*strRes);
      auto strView = StringPrimitive::createStringView(runtime, lv.flagsStr);
      for (char16_t c : strView)
        if (c == u'g')
          isGlobal = true;
      if (!isGlobal)
        return runtime.raiseTypeError(
            "String.prototype.matchAll called with a non-global RegExp argument");
    }
    // c. Let matcher be ? GetMethod(regexp, @@matchAll).
    lv.matchAllSymbol = HermesValue::encodeSymbolValue(
        Predefined::getSymbolID(Predefined::SymbolMatchAll));
    auto matcherRes = getMethod(runtime, regexp, lv.matchAllSymbol);
    if (LLVM_UNLIKELY(matcherRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // d. If matcher is not undefined, then
    if (!matcherRes->getHermesValue().isUndefined()) {
      lv.matcher = std::move(*matcherRes);
      auto matcher = Handle<Callable>::vmcast(&lv.matcher);
      // i. Return ? Call(matcher, regexp, «O»).
      return Callable::executeCall1(
                 matcher, runtime, regexp, O.getHermesValue())
          .toCallResultHermesValue();
    }
  }

  // 3. Let S be ? ToString(O).
  auto strRes = toString_RJS(runtime, O);
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);

  // 4. Let rx be ? RegExpCreate(regexp, "g").
  lv.flagsStr = runtime.getCharacterString('g');
  auto regRes = regExpCreate(
      runtime, regexp, lv.flagsStr, MutableHandle<JSRegExp>{lv.rx});
  if (regRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  // 5. Return ? Invoke(rx, @@matchAll, «S»).
  auto propRes = JSObject::getNamed_RJS(
      lv.rx, runtime, Predefined::getSymbolID(Predefined::SymbolMatchAll));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.propValue = std::move(*propRes);
  auto func = Handle<Callable>::dyn_vmcast(Handle<>{lv.propValue});
  if (LLVM_UNLIKELY(!func)) {
    return runtime.raiseTypeError(
        "RegExp.prototype[@@matchAll] must be callable.");
  }
  return Callable::executeCall1(func, runtime, lv.rx, lv.S.getHermesValue())
      .toCallResultHermesValue();
}

/// This provides a shared implementation of three operations in ES2021:
/// 6.1.4.1 Runtime Semantics: StringIndexOf ( string, searchValue, fromIndex )
///   when clampPostion=false,
/// 21.1.3.8 String.prototype.indexOf ( searchString [ , position ] )
///   when reverse=false, and
/// 21.1.3.9 String.prototype.lastIndexOf ( searchString [ , position ] )
///   when reverse=true.
///
/// Given a haystack ('string'), needle ('searchString'), and position, return
/// the index of the first (reverse=false) or last (reverse=true) substring
/// match of needle within haystack that is not smaller (normal) or larger
/// (reverse) than position.
///
/// \param runtime  the runtime to use for argument coercions
/// \param string  represent the string searching from, i.e. "this" of
///   indexOf / lastIndexOf or "string" of StringIndexOf.
/// \param searchString  represent the substring searching for, i.e.
///   "searchString" of indexOf / lastIndexOf or "searchValue" of StringIndexOf.
/// \param position  represent the starting index of the search, i.e.
///   "position" of indexOf / lastIndexOf or "fromIndex" of StringIndexOf.
/// \param reverse  whether we are running lastIndexOf (true) or indexOf (false)
/// \param clampPosition  whether the "position" is clamped to [0, length)
///   (true if running indexOf / lastIndexOf) or not (running StringIndexOf).
/// \returns Hermes-encoded index of the substring match, or -1 on failure
static CallResult<HermesValue> stringDirectedIndexOf(
    Runtime &runtime,
    Handle<> string,
    Handle<> searchString,
    Handle<> position,
    bool reverse,
    bool clampPosition = true) {
  struct : public Locals {
    PinnedValue<StringPrimitive> S;
    PinnedValue<StringPrimitive> searchStr;
    PinnedValue<> numPos;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  // 1. Let O be ? RequireObjectCoercible(this value).
  // Call a function that may throw, let the runtime record it.
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, string) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 2. Let S be ? ToString(O).
  auto strRes = toString_RJS(runtime, string);
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);

  // 3. Let searchStr be ? ToString(searchString).
  auto searchStrRes = toString_RJS(runtime, searchString);
  if (searchStrRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.searchStr = std::move(*searchStrRes);

  double pos;
  if (reverse) {
    // lastIndexOf
    // 4. Let numPos be ? ToNumber(position).
    auto intRes = toNumber_RJS(runtime, position);
    if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.numPos = intRes.getValue();
    // 6. If numPos is NaN, let pos be +∞; otherwise, let pos be !
    // ToIntegerOrInfinity(numPos).
    if (std::isnan(lv.numPos->getNumber())) {
      pos = std::numeric_limits<double>::infinity();
    } else {
      if (LLVM_UNLIKELY(
              (intRes = toIntegerOrInfinity(runtime, lv.numPos)) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      pos = intRes->getNumber();
    }
  } else {
    // indexOf
    // 4. Let pos be ? ToIntegerOrInfinity(position).
    auto intRes = toIntegerOrInfinity(runtime, position);
    if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    pos = intRes->getNumber();
  }

  // Let len be the length of S.
  double len = lv.S->getStringLength();

  // When pos > len and "searchString" is an empty string, StringIndexOf behaves
  // differently than String.prototype.indexOf in terms of the "clampPosition":
  //   'aa'.indexOf('', 3) => 2               3 is clamped to 2.
  //    StringIndexOf('aa', '', 3) => -1      3 is not clamped thus -1.
  // Also, when pos > len and "searchString" is non-empty, they both fail.
  // Therefore, it's safe to early return -1 for the case of StringIndexOf
  // (i.e. clampPosition=false) as soon as pos > len is observed.
  if (!clampPosition && pos > len) {
    return HermesValue::encodeTrustedNumberValue(-1);
  }

  // Let start be min(max(pos, 0), len).
  uint32_t start = static_cast<uint32_t>(std::min(std::max(pos, 0.), len));

  // TODO: good candidate for Boyer-Moore on large needles/haystacks
  // TODO: good candidate for memchr on length-1 needles
  auto SView = StringPrimitive::createStringView(runtime, lv.S);
  auto searchStrView = StringPrimitive::createStringView(runtime, lv.searchStr);
  double ret = -1;
  if (reverse) {
    // lastIndexOf
    uint32_t lastPossibleMatchEnd =
        std::min(SView.length(), start + searchStrView.length());
    auto foundIter = std::search(
        SView.rbegin() + (SView.length() - lastPossibleMatchEnd),
        SView.rend(),
        searchStrView.rbegin(),
        searchStrView.rend());
    if (foundIter != SView.rend() || searchStrView.empty()) {
      ret = SView.rend() - foundIter - searchStrView.length();
    }
  } else {
    // indexOf
    auto foundIter = std::search(
        SView.begin() + start,
        SView.end(),
        searchStrView.begin(),
        searchStrView.end());
    if (foundIter != SView.end() || searchStrView.empty()) {
      ret = foundIter - SView.begin();
    }
  }
  return HermesValue::encodeTrustedNumberValue(ret);
}

/// ES12 6.1.4.1 Runtime Semantics: StringIndexOf ( string, searchValue,
/// fromIndex )
/// This is currently implemented as a wrapper of stringDirectedIndexOf.
/// Ideally, this can be implemented with less runtime checks and provide a fast
/// path than stringDirectedIndexOf. TODO(T74338730)
static CallResult<HermesValue> stringIndexOf(
    Runtime &runtime,
    Handle<StringPrimitive> string,
    Handle<StringPrimitive> searchValue,
    uint32_t fromIndex) {
  struct : public Locals {
    PinnedValue<> fromIndexHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 1. Assert: Type(string) is String.
  // 2. Assert: Type(searchValue) is String.
  // 3. Assert: ! IsNonNegativeInteger(fromIndex) is true.
  lv.fromIndexHandle = HermesValue::encodeTrustedNumberValue(fromIndex);
  return stringDirectedIndexOf(
      runtime, string, searchValue, Handle<>{lv.fromIndexHandle}, false, false);
}

/// ES12 21.1.3.18 String.prototype.replaceAll ( searchValue, replaceValue )
CallResult<HermesValue> stringPrototypeReplaceAll(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};

  struct : public Locals {
    PinnedValue<> flags;
    PinnedValue<StringPrimitive> flagsStr;
    PinnedValue<> replaceSymbol;
    PinnedValue<> replacer;
    PinnedValue<StringPrimitive> string;
    PinnedValue<StringPrimitive> searchString;
    PinnedValue<StringPrimitive> replaceValueStr;
    PinnedValue<StringPrimitive> replacement;
    PinnedValue<> replacementCallRes;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // 1. Let O be ? RequireObjectCoercible(this value).
  auto O = args.getThisHandle();
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, O) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  auto searchValue = args.getArgHandle(0);
  auto replaceValue = args.getArgHandle(1);
  // 2. If searchValue is an Object, then
  if (searchValue->isObject()) {
    // a. Let isRegExp be ? IsRegExp(searchValue).
    auto isRegExpRes = isRegExp(runtime, searchValue);
    if (LLVM_UNLIKELY(isRegExpRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // b. If isRegExp is true, then
    if (*isRegExpRes) {
      Handle<JSObject> regexpObj = Handle<JSObject>::vmcast(searchValue);
      // i. Let flags be ? Get(searchValue, "flags").
      auto flagsPropRes = JSObject::getNamed_RJS(
          regexpObj, runtime, Predefined::getSymbolID(Predefined::flags));
      if (LLVM_UNLIKELY(flagsPropRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.flags = std::move(*flagsPropRes);
      // ii. Perform ? RequireObjectCoercible(flags).
      if (LLVM_UNLIKELY(
              checkObjectCoercible(runtime, lv.flags) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      // iii. If ? ToString(flags) does not contain "g", throw a TypeError
      // exception.
      auto strRes = toString_RJS(runtime, lv.flags);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.flagsStr = std::move(*strRes);
      auto strView = StringPrimitive::createStringView(runtime, lv.flagsStr);
      bool isGlobal = false;
      for (char16_t c : strView)
        if (c == u'g')
          isGlobal = true;
      if (!isGlobal)
        return runtime.raiseTypeError(
            "String.prototype.replaceAll called with a non-global RegExp argument");
    }
    // c. Let replacer be ? GetMethod(searchValue, @@replace).
    lv.replaceSymbol = HermesValue::encodeSymbolValue(
        Predefined::getSymbolID(Predefined::SymbolReplace));
    auto replacerRes = getMethod(runtime, searchValue, lv.replaceSymbol);
    if (LLVM_UNLIKELY(replacerRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // d. If replacer is not undefined, then
    if (!replacerRes->getHermesValue().isUndefined()) {
      lv.replacer = std::move(*replacerRes);
      // i. Return ? Call(replacer, searchValue, « O, replaceValue »)
      return Callable::executeCall2(
                 Handle<Callable>::vmcast(Handle<>{lv.replacer}),
                 runtime,
                 searchValue,
                 O.getHermesValue(),
                 replaceValue.getHermesValue())
          .toCallResultHermesValue();
    }
  }

  // 3. Let string be ? ToString(O).
  auto strRes = toString_RJS(runtime, O);
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.string = std::move(*strRes);

  // 4. Let searchString be ? ToString(searchValue).
  auto searchStrRes = toString_RJS(runtime, searchValue);
  if (LLVM_UNLIKELY(searchStrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.searchString = std::move(*searchStrRes);

  // 5. Let functionalReplace be IsCallable(replaceValue).
  auto replaceFn = Handle<Callable>::dyn_vmcast(replaceValue);
  bool functionalReplace = !!replaceFn;

  // 6. If functionalReplace is false, then
  if (!functionalReplace) {
    // a. Set replaceValue to ? ToString(replaceValue).
    auto replaceValueStrRes = toString_RJS(runtime, replaceValue);
    if (LLVM_UNLIKELY(replaceValueStrRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.replaceValueStr = std::move(*replaceValueStrRes);
  }

  // 7. Let searchLength be the length of searchString.
  uint32_t searchLength = lv.searchString->getStringLength();
  // 8. Let advanceBy be max(1, searchLength).
  uint32_t advanceBy = std::max(1u, searchLength);

  // 9. Let matchPositions be a new empty List.
  llvh::SmallVector<int32_t, 8> matchPositions{};

  // 10. Let position be ! StringIndexOf(string, searchString, 0).
  auto positionRes = stringIndexOf(runtime, lv.string, lv.searchString, 0);
  int32_t position = positionRes->getNumberAs<int32_t>();

  // 11. Repeat, while position is not -1,
  while (position != -1) {
    GCScopeMarkerRAII marker{runtime};
    // a. Append position to the end of matchPositions.
    matchPositions.push_back(position);
    // b. Set position to ! StringIndexOf(string, searchString, position +
    // advanceBy).
    positionRes = stringIndexOf(
        runtime, lv.string, lv.searchString, position + advanceBy);
    assert(
        positionRes == ExecutionStatus::RETURNED &&
        "StringIndexOf cannot fail");
    position = positionRes->getNumberAs<int32_t>();
  }

  // 12. Let endOfLastMatch be 0.
  uint32_t endOfLastMatch = 0;
  // 13. Let result be the empty String value.
  SmallU16String<32> result{};

  // 14. For each position in matchPositions, do
  auto stringView = StringPrimitive::createStringView(runtime, lv.string);
  for (uint32_t i = 0, size = matchPositions.size(); i < size; ++i) {
    GCScopeMarkerRAII marker{runtime};
    uint32_t position = matchPositions[i];
    // a. Let preserved be the substring of string from endOfLastMatch to
    // position.
    // Noted that "substring" is from inclusiveStart to exclusiveEnd.
    auto preserved =
        stringView.slice(endOfLastMatch, position - endOfLastMatch);
    // b. If functionalReplace is true, then
    if (functionalReplace) {
      // i. Let replacement be ? ToString(?
      //   Call(replaceValue, undefined, « searchString, position, string »)).
      auto callRes = Callable::executeCall3(
                         replaceFn,
                         runtime,
                         Runtime::getUndefinedValue(),
                         lv.searchString.getHermesValue(),
                         HermesValue::encodeTrustedNumberValue(position),
                         lv.string.getHermesValue())
                         .toCallResultHermesValue();
      if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.replacementCallRes = *callRes;
      auto replacementStrRes = toString_RJS(runtime, lv.replacementCallRes);
      if (LLVM_UNLIKELY(replacementStrRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.replacement = std::move(*replacementStrRes);
    } else {
      // c. Else,
      // i. Assert: Type(replaceValue) is String.
      // ii. Let captures be a new empty List.
      auto captures = Runtime::makeNullHandle<ArrayStorageSmall>();
      auto namedCaptures = Runtime::makeNullHandle<JSObject>();
      // iii. Let replacement be ! GetSubstitution(searchString, string,
      // position, captures, undefined, replaceValue).
      auto callRes = getSubstitution(
          runtime,
          lv.searchString,
          lv.string,
          (double)position,
          captures,
          namedCaptures,
          lv.replaceValueStr);
      if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.replacement = vmcast<StringPrimitive>(*callRes);
    }

    // d. Set result to the string-concatenation of result, preserved, and
    // replacement.
    preserved.appendUTF16String(result);
    StringPrimitive::createStringView(runtime, lv.replacement)
        .appendUTF16String(result);
    // e. Set endOfLastMatch to position + searchLength.
    endOfLastMatch = position + searchLength;
  }

  // 15. If endOfLastMatch < the length of string, then
  if (endOfLastMatch < lv.string->getStringLength()) {
    // a. Set result to the string-concatenation of result and the substring of
    // string from endOfLastMatch.
    stringView.slice(endOfLastMatch).appendUTF16String(result);
  }
  // 16. Return result.
  return StringPrimitive::create(runtime, result);
}

CallResult<HermesValue> stringPrototypeMatch(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  struct : public Locals {
    PinnedValue<> matchSymbol;
    PinnedValue<> matcher;
    PinnedValue<StringPrimitive> S;
    PinnedValue<JSRegExp> rx;
    PinnedValue<> propValue;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  // 1. Let O be RequireObjectCoercible(this value).
  // 2. ReturnIfAbrupt(O).
  auto O = args.getThisHandle();
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, O) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 3. If regexp is an Object, then
  auto regexp = args.getArgHandle(0);
  if (regexp->isObject()) {
    // a. Let matcher be GetMethod(regexp, @@match).
    lv.matchSymbol = HermesValue::encodeSymbolValue(
        Predefined::getSymbolID(Predefined::SymbolMatch));
    auto methodRes = getMethod(runtime, regexp, lv.matchSymbol);
    // b. ReturnIfAbrupt(matcher).
    if (LLVM_UNLIKELY(methodRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // c. If matcher is not undefined, then
    //   i. Return Call(matcher, regexp, «‍O»).
    if (!methodRes->getHermesValue().isUndefined()) {
      lv.matcher = std::move(*methodRes);
      auto matcher = Handle<Callable>::vmcast(&lv.matcher);
      return Callable::executeCall1(
                 matcher, runtime, regexp, O.getHermesValue())
          .toCallResultHermesValue();
    }
  }
  // 4. Let S be ToString(O).
  auto strRes = toString_RJS(runtime, O);
  // 5. ReturnIfAbrupt(S).
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);

  // 6. Let rx be RegExpCreate(regexp, undefined) (see 21.2.3.2.3).
  // 7. ReturnIfAbrupt(rx).
  auto regRes = regExpCreate(
      runtime,
      regexp,
      Runtime::getUndefinedValue(),
      MutableHandle<JSRegExp>{lv.rx});
  if (regRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  // 8. Return Invoke(rx, @@match, «‍S»).
  auto propRes = JSObject::getNamed_RJS(
      lv.rx, runtime, Predefined::getSymbolID(Predefined::SymbolMatch));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.propValue = std::move(*propRes);
  auto func = Handle<Callable>::dyn_vmcast(Handle<>{lv.propValue});
  if (LLVM_UNLIKELY(!func)) {
    return runtime.raiseTypeError(
        "RegExp.prototype[@@match] must be callable.");
  }
  return Callable::executeCall1(func, runtime, lv.rx, lv.S.getHermesValue())
      .toCallResultHermesValue();
}

CallResult<HermesValue> stringPrototypePad(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  bool padStart = (bool)ctx;
  struct : public Locals {
    PinnedValue<StringPrimitive> S;
    PinnedValue<StringPrimitive> filler;
    PinnedValue<StringPrimitive> result;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 1. Let O be ? RequireObjectCoercible(this value).
  auto O = args.getThisHandle();
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, O) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 2. Let S be ? ToString(O).
  auto sRes = toString_RJS(runtime, O);
  if (LLVM_UNLIKELY(sRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*sRes);

  // 3. Let intMaxLength be ? ToLength(maxLength).
  auto intMaxLengthRes = toLength(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(intMaxLengthRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  const uint64_t intMaxLength = intMaxLengthRes->getNumberAs<int64_t>();

  // 4. Let stringLength be the number of elements in S.
  const uint32_t stringLength = lv.S->getStringLength();
  SafeUInt32 size{stringLength};

  // 5. If intMaxLength is not greater than stringLength, return S.
  if (intMaxLength <= stringLength) {
    return lv.S.getHermesValue();
  }

  if (args.getArg(1).isUndefined()) {
    // 6. If fillString is undefined, let filler be a String consisting solely
    // of the code unit 0x0020 (SPACE).
    lv.filler = runtime.getPredefinedString(Predefined::space);
  } else {
    // 7. Else, let filler be ? ToString(fillString).
    auto fillerRes = toString_RJS(runtime, args.getArgHandle(1));
    if (LLVM_UNLIKELY(fillerRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.filler = fillerRes->get();
  }

  // 8. If filler is the empty String, return S.
  if (lv.filler->getStringLength() == 0) {
    return lv.S.getHermesValue();
  }

  // 9. Let fillLen be intMaxLength - stringLength.
  const uint64_t fillLen = intMaxLength - stringLength;

  // Check for overflow and strings that are too long, so we can ensure uint32_t
  // is a large enough type to use for math below.
  if (fillLen > StringPrimitive::MAX_STRING_LENGTH) {
    return runtime.raiseRangeError("String pad result exceeds limit");
  }

  size.add((uint32_t)fillLen);

  if (size.isZero()) {
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::emptyString));
  }

  auto builderRes = StringBuilder::createStringBuilder(
      runtime, lv.result, size, lv.S->isASCII() && lv.filler->isASCII());
  if (LLVM_UNLIKELY(builderRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  uint32_t resultLen = size.get();

  // Repeatedly add filler to builder, taking up resultLen - stringLength
  // characters.
  auto addFiller = [&lv, resultLen, stringLength](StringBuilder &builder) {
    uint32_t remaining = resultLen - stringLength;
    const uint32_t fillerLen = lv.filler->getStringLength();
    while (remaining != 0) {
      uint32_t length = std::min(remaining, fillerLen);
      builder.appendStringPrim(lv.filler, length);
      remaining -= length;
    }
  };

  if (padStart) {
    // 10. Let truncatedStringFiller be a new String value consisting of
    // repeated concatenations of filler truncated to length fillLen.
    // 11. Return a new String value computed by the concatenation of
    // truncatedStringFiller and S.
    addFiller(*builderRes);
    builderRes->appendStringPrim(lv.S);
  } else {
    // 10. Let truncatedStringFiller be a new String value consisting of
    // repeated concatenations of filler truncated to length fillLen.
    // 11. Return a new String value computed by the concatenation of S and
    // truncatedStringFiller.
    builderRes->appendStringPrim(lv.S);
    addFiller(*builderRes);
  }

  return builderRes->getStringPrimitive().getHermesValue();
}

CallResult<HermesValue> stringPrototypeReplace(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  struct : public Locals {
    PinnedValue<> replaceSymbol;
    PinnedValue<StringPrimitive> string;
    PinnedValue<StringPrimitive> searchString;
    PinnedValue<> callRes;
    PinnedValue<StringPrimitive> replaceValueStr;
    PinnedValue<StringPrimitive> replStr;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  // 1. Let O be RequireObjectCoercible(this value).
  // 2. ReturnIfAbrupt(O).
  auto O = args.getThisHandle();
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, O) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 3. If searchValue is an Object, then
  auto searchValue = args.getArgHandle(0);
  auto replaceValue = args.getArgHandle(1);
  if (searchValue->isObject()) {
    // a. Let replacer be GetMethod(searchValue, @@replace).
    lv.replaceSymbol = HermesValue::encodeSymbolValue(
        Predefined::getSymbolID(Predefined::SymbolReplace));
    auto methodRes = getMethod(runtime, searchValue, lv.replaceSymbol);
    // b. ReturnIfAbrupt(replacer).
    if (LLVM_UNLIKELY(methodRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // c. If replacer is not undefined, then
    //   i. Return Call(replacer, searchValue, «‍O, replaceValue»).
    if (!methodRes->getHermesValue().isUndefined()) {
      // If methodRes is not Callable, step 3a would have thrown a TypeError.
      Handle<Callable> replacer =
          Handle<Callable>::vmcast(runtime, methodRes->getHermesValue());
      return Callable::executeCall2(
                 replacer,
                 runtime,
                 searchValue,
                 O.getHermesValue(),
                 replaceValue.getHermesValue())
          .toCallResultHermesValue();
    }
  }
  // 4. Let string be ToString(O).
  // 5. ReturnIfAbrupt(string).
  auto stringRes = toString_RJS(runtime, O);
  if (LLVM_UNLIKELY(stringRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.string = std::move(*stringRes);
  // 6. Let searchString be ToString(searchValue).
  auto searchStringRes = toString_RJS(runtime, searchValue);
  // 7. ReturnIfAbrupt(searchString).
  if (LLVM_UNLIKELY(searchStringRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.searchString = std::move(*searchStringRes);
  // 8. Let functionalReplace be IsCallable(replaceValue).
  auto replaceFn = Handle<Callable>::dyn_vmcast(replaceValue);
  bool functionalReplace = !!replaceFn;
  // 9. If functionalReplace is false, then
  if (!functionalReplace) {
    // a. Let replaceValue be ToString(replaceValue).
    // b. ReturnIfAbrupt(replaceValue).
    auto replaceValueStrRes = toString_RJS(runtime, replaceValue);
    if (LLVM_UNLIKELY(replaceValueStrRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.replaceValueStr = std::move(*replaceValueStrRes);
  }
  // 10. Search string for the first occurrence of searchString and let pos be
  // the index within string of the first code unit of the matched substring and
  // let matched be searchString. If no occurrences of searchString were found,
  // return string.
  // Special case: if they're both empty then the match is at position 0.
  uint32_t pos = 0;
  auto strView = StringPrimitive::createStringView(runtime, lv.string);
  if (!strView.empty()) {
    auto searchView =
        StringPrimitive::createStringView(runtime, lv.searchString);
    auto searchResult = std::search(
        strView.begin(), strView.end(), searchView.begin(), searchView.end());

    if (searchResult != strView.end()) {
      pos = searchResult - strView.begin();
    } else {
      return lv.string.getHermesValue();
    }
  } else if (lv.searchString->getStringLength() != 0) {
    // If string is empty and search is not empty, there is no match.
    return lv.string.getHermesValue();
  }
  // 11. If functionalReplace is true, then
  if (functionalReplace) {
    // a. Let replValue be Call(replaceValue, undefined, «matched, pos, and
    // string»).
    auto callRes = Callable::executeCall3(
        replaceFn,
        runtime,
        Runtime::getUndefinedValue(),
        lv.searchString.getHermesValue(),
        HermesValue::encodeTrustedNumberValue(pos),
        lv.string.getHermesValue());
    if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // b. Let replStr be ToString(replValue).
    lv.callRes = std::move(*callRes);
    auto replStrRes = toString_RJS(runtime, lv.callRes);
    // c. ReturnIfAbrupt(replStr).
    if (LLVM_UNLIKELY(replStrRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.replStr = replStrRes->get();
  } else {
    // 12. Else,
    // a. Let captures be an empty List.
    auto nullHandle = Runtime::makeNullHandle<ArrayStorageSmall>();
    auto namedCaptures = Runtime::makeNullHandle<JSObject>();
    // b. Let replStr be GetSubstitution(matched, string, pos, captures,
    // replaceValue).
    auto callRes = getSubstitution(
        runtime,
        lv.searchString,
        lv.string,
        pos,
        nullHandle,
        namedCaptures,
        lv.replaceValueStr);
    if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.replStr = vmcast<StringPrimitive>(callRes.getValue());
  }
  // 13. Let tailPos be pos + the number of code units in matched.
  uint32_t tailPos = pos + lv.searchString->getStringLength();
  // 14. Let newString be the String formed by concatenating the first pos code
  // units of string, replStr, and the trailing substring of string starting at
  // index tailPos. If pos is 0, the first element of the concatenation will be
  // the empty String.
  SmallU16String<32> newString{};
  strView.slice(0, pos).appendUTF16String(newString);
  StringPrimitive::createStringView(runtime, lv.replStr)
      .appendUTF16String(newString);
  strView.slice(tailPos).appendUTF16String(newString);
  // 15. Return newString.
  return StringPrimitive::create(runtime, newString);
}

CallResult<HermesValue> stringPrototypeSearch(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  struct : public Locals {
    PinnedValue<> searchSymbol;
    PinnedValue<StringPrimitive> string;
    PinnedValue<JSRegExp> rx;
    PinnedValue<> propValue;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  // 1. Let O be RequireObjectCoercible(this value).
  // 2. ReturnIfAbrupt(O).
  auto O = args.getThisHandle();
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, O) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 3. If regexp is an Object, then
  auto regexp = args.getArgHandle(0);
  if (regexp->isObject()) {
    // a. Let searcher be GetMethod(regexp, @@search).
    lv.searchSymbol = HermesValue::encodeSymbolValue(
        Predefined::getSymbolID(Predefined::SymbolSearch));
    auto methodRes = getMethod(runtime, regexp, lv.searchSymbol);
    // b. ReturnIfAbrupt(searcher).
    if (LLVM_UNLIKELY(methodRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // c. If searcher is not undefined, then
    //   i. Return Call(searcher, regexp, «‍O»).
    if (!methodRes->getHermesValue().isUndefined()) {
      // If methodRes is not Callable, step 3a would have thrown a TypeError.
      Handle<Callable> searcher =
          Handle<Callable>::vmcast(runtime, methodRes->getHermesValue());
      return Callable::executeCall1(
                 searcher, runtime, regexp, O.getHermesValue())
          .toCallResultHermesValue();
    }
  }
  // 4. Let string be ToString(O).
  // 5. ReturnIfAbrupt(string).
  auto strRes = toString_RJS(runtime, O);
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.string = std::move(*strRes);

  // 6. Let rx be RegExpCreate(regexp, undefined) (see 21.2.3.2.3).
  // 7. ReturnIfAbrupt(rx).
  auto regRes = regExpCreate(
      runtime,
      regexp,
      Runtime::getUndefinedValue(),
      MutableHandle<JSRegExp>{lv.rx});
  if (regRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  // 8. Return Invoke(rx, @@search, «string»).
  auto propRes = JSObject::getNamed_RJS(
      lv.rx, runtime, Predefined::getSymbolID(Predefined::SymbolSearch));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_UNLIKELY(!vmisa<Callable>(propRes->get()))) {
    return runtime.raiseTypeError(
        "RegExp.prototype[@@search] must be callable.");
  }
  lv.propValue = std::move(*propRes);
  auto func = Handle<Callable>::vmcast(&lv.propValue);
  return Callable::executeCall1(
             func, runtime, lv.rx, lv.string.getHermesValue())
      .toCallResultHermesValue();
}

CallResult<HermesValue> stringPrototypeCharAt(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  Handle<> thisValue{&args.getThisArg()};
  struct : public Locals {
    PinnedValue<StringPrimitive> S;
    PinnedValue<> argHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Call a function that may throw, let the runtime record it.
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, thisValue) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto strRes = toString_RJS(runtime, thisValue);
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);

  lv.argHandle = args.getArg(0);
  auto intRes = toIntegerOrInfinity(runtime, Handle<>{lv.argHandle});
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto position = intRes->getNumber();
  auto size = lv.S->getStringLength();
  if (position < 0 || position >= size) {
    return HermesValue::encodeStringValue(
        runtime.getPredefinedString(Predefined::emptyString));
  }
  return runtime
      .getCharacterString(
          StringPrimitive::createStringView(runtime, lv.S)[position])
      .getHermesValue();
}

CallResult<HermesValue> stringPrototypeSlice(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  struct : public Locals {
    PinnedValue<StringPrimitive> S;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, args.getThisHandle()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto strRes = toString_RJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);
  double len = lv.S->getStringLength();

  auto intRes = toIntegerOrInfinity(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double intStart = intRes->getNumber();

  double intEnd;
  if (args.getArg(1).isUndefined()) {
    intEnd = len;
  } else {
    if (LLVM_UNLIKELY(
            (intRes = toIntegerOrInfinity(runtime, args.getArgHandle(1))) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    intEnd = intRes->getNumber();
  }

  size_t from =
      intStart < 0 ? std::max(len + intStart, 0.0) : std::min(intStart, len);
  size_t to = intEnd < 0 ? std::max(len + intEnd, 0.0) : std::min(intEnd, len);
  size_t span = to > from ? to - from : 0;
  assert(from + span <= len && "invalid index computed in slice");

  return StringPrimitive::slice(runtime, lv.S, from, span);
}

CallResult<HermesValue> stringPrototypeEndsWith(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  struct : public Locals {
    PinnedValue<StringPrimitive> S;
    PinnedValue<StringPrimitive> searchStr;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // 1. Let O be RequireObjectCoercible(this value).
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, args.getThisHandle()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 2. Let S be ToString(O).
  auto strRes = toString_RJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);

  // 4. Let isRegExp be IsRegExp(searchString).
  auto isRegExpRes = isRegExp(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(isRegExpRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 6. If isRegExp is true, throw a TypeError exception.
  if (LLVM_UNLIKELY(*isRegExpRes)) {
    return runtime.raiseTypeError(
        "First argument to endsWith must not be a RegExp");
  }

  // 7. Let searchStr be ToString(searchString).
  auto searchStrRes = toString_RJS(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(searchStrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.searchStr = std::move(*searchStrRes);

  // 9. Let len be the number of elements in S.
  double len = lv.S->getStringLength();

  // 10. If endPosition is undefined, let pos be len, else let pos be
  // ToIntegerOrInfinity(endPosition).
  double pos;
  if (args.getArg(1).isUndefined()) {
    pos = len;
  } else {
    auto posRes = toIntegerOrInfinity(runtime, args.getArgHandle(1));
    if (LLVM_UNLIKELY(posRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    pos = posRes->getNumberAs<double>();
  }

  // 12. Let end be min(max(pos, 0), len).
  double end = std::min(std::max(pos, 0.0), len);

  // 13. Let searchLength be the number of elements in searchStr.
  double searchLength = lv.searchStr->getStringLength();

  // 14. Let start be end - searchLength.
  double start = end - searchLength;

  // 15. If start is less than 0, return false.
  if (start < 0) {
    return HermesValue::encodeBoolValue(false);
  }
  // 16. If the sequence of elements of S starting at start of length
  // searchLength is the same as the full element sequence of searchStr, return
  // true.
  // 17. Otherwise, return false.
  return HermesValue::encodeBoolValue(
      lv.S->sliceEquals(start, searchLength, *lv.searchStr));
}

/// ES11 21.1.3.20.1
/// Works slightly differently from the given implementation in the spec.
/// Given a string \p S and a starting point \p q, finds the first match of
/// \p R such that it starts on or after index \p q in \p S.
/// \param q starting point in S. Requires: q <= S->getStringLength().
/// \param R a String.
/// \return Either None or an integer representing the start of the match.
static OptValue<uint32_t> splitMatch(
    Runtime &runtime,
    Handle<StringPrimitive> S,
    uint32_t q,
    Handle<StringPrimitive> R) {
  // 2. Let r be the number of code units in R.
  auto r = R->getStringLength();
  // 3. Let s be the number of code units in S.
  auto s = S->getStringLength();
  // 4. If q+r > s, return false.
  if (q + r > s) {
    return llvh::None;
  }

  // Handle the case where the search starts at the end of the string and R is
  // the empty string. This path should only be triggered when S is itself the
  // empty string.
  if (q == s) {
    return q;
  }

  auto SStr = StringPrimitive::createStringView(runtime, S);
  auto RStr = StringPrimitive::createStringView(runtime, R);

  auto sliced = SStr.slice(q);
  auto searchResult =
      std::search(sliced.begin(), sliced.end(), RStr.begin(), RStr.end());

  if (searchResult != sliced.end()) {
    return q + (searchResult - sliced.begin()) + r;
  }
  return llvh::None;
}

/// String.prototype.split() fast path when the separator is the empty string.
static CallResult<HermesValue> splitEmptySep(
    Runtime &runtime,
    Handle<StringPrimitive> S,
    Handle<JSArray> A,
    uint32_t lim) {
  uint32_t len = std::min(S->getStringLength(), lim);

  // Resize the array storage: one element per character.
  if (LLVM_UNLIKELY(
          JSArray::setStorageEndIndex(A, runtime, len) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // Update the .length property correspondingly.
  if (LLVM_UNLIKELY(
          JSArray::setLengthProperty(A, runtime, len) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (S->isASCII()) {
    // ASCII path with zero allocations!
    NoAllocScope noAlloc{runtime};
    JSArray *arr = A.get();
    const char *asciiPtr = S->getStringRef<char>().data();

    for (uint32_t i = 0; i != len; ++asciiPtr, ++i) {
      auto str = SmallHermesValue::encodeStringValue(
          runtime.getCharacterString((uint8_t)*asciiPtr).get(), runtime);
      JSArray::unsafeSetExistingElementAt(arr, runtime, i, str);
    }
  } else {
    // UTF16 path with allocations.
    GCScopeMarkerRAII marker{runtime};
    for (uint32_t i = 0; i != len; ++i) {
      marker.flush();
      auto str = SmallHermesValue::encodeStringValue(
          runtime.getCharacterString(S->at(i)).get(), runtime);
      JSArray::unsafeSetExistingElementAt(A.get(), runtime, i, str);
    }
  }

  return A.getHermesValue();
}

// ES11 21.1.3.20 String.prototype.split ( separator, limit )
CallResult<HermesValue> stringPrototypeSplit(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};

  struct : public Locals {
    PinnedValue<> splitSymbol;
    PinnedValue<StringPrimitive> S;
    PinnedValue<JSArray> A;
    PinnedValue<StringPrimitive> R;
    PinnedValue<> tmpHandle;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // 1. Let O be ? RequireObjectCoercible(this value).
  auto O = args.getThisHandle();
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, O) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 2. If separator is an Object, then
  auto separator = args.getArgHandle(0);
  if (separator->isObject()) {
    // a. Let splitter be ? GetMethod(separator, @@split).
    lv.splitSymbol = HermesValue::encodeSymbolValue(
        Predefined::getSymbolID(Predefined::SymbolSplit));
    auto methodRes = getMethod(runtime, separator, lv.splitSymbol);
    if (LLVM_UNLIKELY(methodRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // b. If splitter is not undefined, then
    // i. Return ? Call(splitter, separator, «‍O, limit»).
    if (!methodRes->getHermesValue().isUndefined()) {
      Handle<Callable> splitter =
          Handle<Callable>::vmcast(runtime, methodRes->getHermesValue());
      return Callable::executeCall2(
                 splitter,
                 runtime,
                 separator,
                 O.getHermesValue(),
                 args.getArg(1))
          .toCallResultHermesValue();
    }
  }

  // 3. Let S be ? ToString(O).
  auto strRes = toString_RJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);

  // 4. Let A be ! ArrayCreate(0).
  auto arrRes = JSArray::create(runtime, 0, 0);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.A = std::move(*arrRes);
  // 5. Let lengthA be 0.
  uint32_t lengthA = 0;

  // 6. If limit is undefined, let lim be 232 - 1; else let lim be ?
  // ToUint32(limit).
  auto limit = args.getArgHandle(1);
  uint32_t lim;
  if (limit->isUndefined()) {
    // No limit supplied, make it max.
    lim = 0xffffffff; // 2 ^ 32 - 1
  } else {
    auto intRes = toUInt32_RJS(runtime, limit);
    if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lim = intRes->getNumber();
  }

  // 7. Let s be the length of S.
  uint32_t s = lv.S->getStringLength();
  // 8. Let p be 0.
  // End of the last match.
  uint32_t p = 0;

  // 9. Let R be ? ToString(separator).
  // The pattern which we want to separate on.
  auto sepRes = toString_RJS(runtime, separator);
  if (LLVM_UNLIKELY(sepRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.R = std::move(sepRes.getValue());

  // 10. If lim = 0, return A.
  if (lim == 0) {
    // Don't want any elements, so we're done.
    return lv.A.getHermesValue();
  }

  // 11. If separator is undefined, then
  if (LLVM_UNLIKELY(separator->isUndefined())) {
    // a. Perform ! CreateDataPropertyOrThrow(A, "0", S).
    if (LLVM_UNLIKELY(
            JSArray::setElementAt(lv.A, runtime, 0, lv.S) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    if (LLVM_UNLIKELY(
            JSArray::setLengthProperty(lv.A, runtime, 1) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    // b. Return A.
    return lv.A.getHermesValue();
  }

  // 12. If s = 0, then
  if (s == 0) {
    // S is the empty string.
    // a. Let z be SplitMatch(S, 0, R).
    auto matchResult = splitMatch(runtime, lv.S, 0, lv.R);
    // b. If z is not false, return A.
    if (matchResult) {
      return lv.A.getHermesValue();
    }
    // c. Perform ! CreateDataPropertyOrThrow(A, "0", S).
    if (LLVM_UNLIKELY(
            JSArray::setElementAt(lv.A, runtime, 0, lv.S) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    if (LLVM_UNLIKELY(
            JSArray::setLengthProperty(lv.A, runtime, 1) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    // e. Return A.
    return lv.A.getHermesValue();
  }

  // Special case for .split('').
  if (lv.R->getStringLength() == 0) {
    return splitEmptySep(runtime, lv.S, lv.A, lim);
  }

  // 17. Let q be p.
  // Place to attempt the start of the next match.
  uint32_t q = p;

  auto marker = gcScope.createMarker();

  // 18. Repeat, while q ≠ s
  // Main loop: continue while we have space to find another match.
  while (q != s) {
    gcScope.flushToMarker(marker);

    // a. Let e be SplitMatch(S, q, R).
    // b. If e is false, let q = q+1.

    // Find the next valid match. We know that q < s.
    // ES6's SplitMatch only finds matches at q, but we find matches at or
    // after q, so if it fails, we know we're done. In effect, we are performing
    // steps (a) and (b) over and over again.
    auto matchResult = splitMatch(runtime, lv.S, q, lv.R);
    // If we did find a match, fast-forward q to the start of that match.
    if (matchResult) {
      q = *matchResult - lv.R->getStringLength();
    }
    if (!matchResult || q >= s) {
      // There's no matches between index q and the end of the string, so we're
      // done searching. Note: This behavior differs from the spec
      // implementation, because we check for matches at or after q. However, in
      // line with the spec, we only count matches that start before the end of
      // the string.
      break;
    }
    // c. Else,
    // Found a match, so go ahead and update e, such that the match is the range
    // [q,e).
    uint32_t e = *matchResult;
    // i. If e = p, set q to q + 1.
    if (e == p) {
      // The end of this match is the same as the end of the last match,
      // so we matched with the empty string.
      // We don't want to match the empty string at this location again,
      // so increment q in order to start the next search at the next position.
      q++;
    }
    // ii. Else,
    else {
      // Found a non-empty string match.
      // Add everything from the last match to the current one to A.
      // This has length q-p because q is the start of the current match,
      // and p was the end (exclusive) of the last match.

      // 1. Let T be the String value equal to the substring of S consisting of
      // the code units at indices p (inclusive) through q (exclusive).
      auto strRes = StringPrimitive::slice(runtime, lv.S, p, q - p);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.tmpHandle = *strRes;
      // 2. Perform ! CreateDataPropertyOrThrow(A, ! ToString(lengthA), T)
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(lv.A, runtime, lengthA, lv.tmpHandle) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      // 3. Set lengthA to lengthA + 1.
      ++lengthA;

      // 4. If lengthA = lim, return A.
      if (lengthA == lim) {
        // Reached the limit, return early.
        if (LLVM_UNLIKELY(
                JSArray::setLengthProperty(lv.A, runtime, lengthA) ==
                ExecutionStatus::EXCEPTION))
          return ExecutionStatus::EXCEPTION;
        return lv.A.getHermesValue();
      }
      // 5. Set p to e.
      // Update p to point to the end of this match, maintaining the
      // invariant that it points to the end of the last match encountered.
      p = e;
      // 6. Set q to p.
      // Start position of the next search is updated to the end of this match.
      q = p;
    }
  }

  // 15. Let T be the String value equal to the substring of S consisting of the
  // code units at indices p (inclusive) through s (exclusive).
  // Add the rest of the string (after the last match) to A.
  auto elementStrRes = StringPrimitive::slice(runtime, lv.S, p, s - p);
  if (LLVM_UNLIKELY(elementStrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.tmpHandle = *elementStrRes;
  // 16. Perform ! CreateDataPropertyOrThrow(A, ! ToString(lengthA), T).
  if (LLVM_UNLIKELY(
          JSArray::setElementAt(lv.A, runtime, lengthA, lv.tmpHandle) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  ++lengthA;

  if (LLVM_UNLIKELY(
          JSArray::setLengthProperty(lv.A, runtime, lengthA) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;

  // 27. Return A.
  return lv.A.getHermesValue();
}

CallResult<HermesValue> stringPrototypeIncludesOrStartsWith(
    void *ctx,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  struct : public Locals {
    PinnedValue<StringPrimitive> S;
    PinnedValue<StringPrimitive> searchStr;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  // If true, perform the startsWith operation.
  // Else, do the standard "includes" operation.
  bool startsWith = static_cast<bool>(ctx);

  // 1. Let O be RequireObjectCoercible(this value).
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, args.getThisHandle()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 2. Let S be ToString(O).
  auto strRes = toString_RJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);

  // 4. Let isRegExp be IsRegExp(searchString).
  // 6. If isRegExp is true, throw a TypeError exception.
  auto isRegExpRes = isRegExp(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(isRegExpRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  if (LLVM_UNLIKELY(*isRegExpRes)) {
    return runtime.raiseTypeError(
        "First argument to startsWith and includes must not be a RegExp");
  }

  // 7. Let searchStr be ToString(searchString).
  auto searchStrRes = toString_RJS(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(searchStrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.searchStr = std::move(*searchStrRes);

  // 9. Let pos be ToIntegerOrInfinity(position).
  // (If position is undefined, this step produces the value 0).
  auto posRes = toIntegerOrInfinity(runtime, args.getArgHandle(1));
  if (LLVM_UNLIKELY(posRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double pos = posRes->getNumber();

  // 11. Let len be the number of elements in S.
  double len = lv.S->getStringLength();

  // 12. Let start be min(max(pos, 0), len).
  double start = std::min(std::max(pos, 0.0), len);

  // 13. Let searchLength be the number of elements in searchStr.
  double searchLength = lv.searchStr->getStringLength();

  if (startsWith) {
    // Perform the startsWith operation instead of includes.
    // 14. If searchLength+start is greater than len, return false.
    if (searchLength + start > len) {
      return HermesValue::encodeBoolValue(false);
    }
    // 15. If the sequence of elements of S starting at start of length
    // searchLength is the same as the full element sequence of searchStr,
    // return true.
    // 16. Otherwise, return false.
    return HermesValue::encodeBoolValue(
        lv.S->sliceEquals(start, searchLength, *lv.searchStr));
  }

  // 14. If there exists any integer k not smaller than start such that k +
  // searchLen is not greater than len, and for all nonnegative integers j less
  // than searchLen, the code unit at index k+j of S is the same as the code
  // unit at index j of searchStr, return true; but if there is no such integer
  // k, return false.
  auto SView = StringPrimitive::createStringView(runtime, lv.S);
  auto searchStrView = StringPrimitive::createStringView(runtime, lv.searchStr);
  auto foundIter = std::search(
      SView.begin() + start,
      SView.end(),
      searchStrView.begin(),
      searchStrView.end());
  // Note: searchStrView.empty check is needed in the special case that S is
  // empty, searchStr is empty, and start = 0
  return HermesValue::encodeBoolValue(
      foundIter != SView.end() || searchStrView.empty());
}

CallResult<HermesValue> stringPrototypeIndexOf(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto searchString = args.getArgHandle(0);
  auto position = args.getArgHandle(1);
  return stringDirectedIndexOf(
      runtime, args.getThisHandle(), searchString, position, false);
}

CallResult<HermesValue> stringPrototypeLastIndexOf(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto searchString = args.getArgHandle(0);
  auto position = args.getArgHandle(1);
  return stringDirectedIndexOf(
      runtime, args.getThisHandle(), searchString, position, true);
}

// ES15 22.1.3.10 String.prototype.isWellFormed
CallResult<HermesValue> stringPrototypeIsWellFormed(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. Let O be ? RequireObjectCoercible(this value).
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, args.getThisHandle()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 2. Let S be ? ToString(O).
  auto strRes = toString_RJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 3. Return IsStringWellFormedUnicode(S).
  return HermesValue::encodeBoolValue(isStringWellFormedUnicode(strRes->get()));
}

// ES15 22.1.3.31 String.prototype.toWellFormed
CallResult<HermesValue> stringPrototypeToWellFormed(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  struct : public Locals {
    PinnedValue<StringPrimitive> S;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 1. Let O be ? RequireObjectCoercible(this value).
  if (LLVM_UNLIKELY(
          checkObjectCoercible(runtime, args.getThisHandle()) ==
          ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 2. Let S be ? ToString(O).
  auto strRes = toString_RJS(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);

  if (lv.S->isASCII()) {
    // It's impossible for an ASCII string to contain any surrogates, so early
    // return.
    return HermesValue::encodeStringValue(*lv.S);
  }

  ArrayRef<char16_t> strRef = lv.S->getStringRef<char16_t>();
  // 3. Let strLen be the length of S.
  size_t strLen = strRef.size();
  // 4. Let k be 0.
  size_t k = 0;
  // 5. Let result be the empty String.
  llvh::SmallVector<char16_t, 32> result;
  result.reserve(strLen);
  // Keep track of if we've actually inserted a replacement character.
  bool insertedReplacementCharacter = false;
  // 6. Repeat, while k < strLen,
  for (; k < strLen; ++k) {
    // Deviate from the literal word of the spec slightly since we don't deal
    // with code points directly, but rather code units.
    char16_t ch = strRef[k];
    if (ch >= UNICODE_SURROGATE_FIRST && ch <= UNICODE_SURROGATE_LAST) {
      if (isHighSurrogate(ch)) {
        if (k + 1 < strLen) {
          char16_t next = strRef[k + 1];
          if (isLowSurrogate(next)) {
            // Valid surrogate pair, keep both characters
            result.push_back(ch);
            result.push_back(next);
            ++k; // Skip the low surrogate
            continue;
          }
        }
        // Lone surrogate: high surrogate without low counterpart
        insertedReplacementCharacter = true;
        result.push_back(UNICODE_REPLACEMENT_CHARACTER);
      } else {
        // Lone surrogate: low surrogate without high counterpart
        insertedReplacementCharacter = true;
        result.push_back(UNICODE_REPLACEMENT_CHARACTER);
      }
    } else {
      // Regular character, keep as is
      result.push_back(ch);
    }
  }

  // If no replacement character was inserted, the original string can be used
  // as-is.
  return insertedReplacementCharacter
      ? StringPrimitive::createEfficient(runtime, result)
      : HermesValue::encodeStringValue(*lv.S);
}

} // namespace vm
} // namespace hermes

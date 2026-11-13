/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES5.1 15.10.3 RegExp (Regular Expression) Objects
//===----------------------------------------------------------------------===//

#include "JSLibInternal.h"

#include "hermes/VM/JSObject.h"
#include "hermes/VM/JSRegExpStringIterator.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/SmallXString.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringView.h"

#include "llvh/ADT/ArrayRef.h"
#include "llvh/ADT/StringExtras.h"

namespace hermes {
namespace vm {

/// Internal methods
/// {@

/// 21.2.3.2.1 Runtime Semantics: RegExpAlloc ( newTarget )
static CallResult<HermesValue> regExpAlloc(
    Runtime &runtime,
    Handle<Callable> newTarget);

/// 21.2.3.2.2 Runtime Semantics: RegExpInitialize ( obj, pattern, flags )
static ExecutionStatus regExpInitialize(
    Runtime &runtime,
    Handle<> obj,
    Handle<> pattern,
    Handle<> flags,
    MutableHandle<JSRegExp> regExpOut);

/// ES6.0 21.2.5.2.2
static CallResult<HermesValue> regExpBuiltinExec(
    Runtime &runtime,
    Handle<JSRegExp> R,
    Handle<StringPrimitive> S);

/// @}

// Several RegExp accessors are defined to do particular things when passed the
// RegExp object (which is not itself a RegExp). Centralize that logic here.
static inline bool thisIsRegExpProto(Runtime &runtime, NativeArgs args) {
  return args.dyncastThis<JSObject>().get() == *runtime.regExpPrototype;
}

HermesValue createRegExpConstructor(Runtime &runtime) {
  auto proto = Handle<JSObject>::vmcast(&runtime.regExpPrototype);

  struct : public Locals {
    PinnedValue<NativeConstructor> cons;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  defineSystemConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::RegExp),
      regExpConstructor,
      proto,
      2,
      lv.cons);

  // ES2025 22.2.5.2.1 RegExp.escape ( S )
  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::escape),
      nullptr,
      regExpEscape,
      1);

  runtime.regExpPrototypeExec = defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::exec),
      nullptr,
      regExpPrototypeExec,
      1);

  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::test),
      nullptr,
      regExpPrototypeTest,
      1);

  DefinePropertyFlags dpf = DefinePropertyFlags::getNewNonEnumerableFlags();

  (void)defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::SymbolMatchAll),
      Predefined::getSymbolID(Predefined::squareSymbolMatchAll),
      nullptr,
      regExpPrototypeSymbolMatchAll,
      1,
      dpf);

  // The RegExp prototype and constructors have a variety of getters defined on
  // it; use this helper to define them. Note the context is passed as an
  // intptr_t which we convert to void*.
  auto defineGetter = [&](Handle<JSObject> obj,
                          Predefined::Str sym,
                          NativeFunctionPtr getter,
                          intptr_t ctx = 0) {
    defineAccessor(
        runtime,
        obj,
        Predefined::getSymbolID(sym),
        ctx ? reinterpret_cast<void *>(ctx) : nullptr,
        getter,
        nullptr,
        false,
        true);
  };

  defineGetter(proto, Predefined::source, regExpSourceGetter);
  defineGetter(proto, Predefined::multiline, regExpFlagPropertyGetter, 'm');
  defineGetter(proto, Predefined::ignoreCase, regExpFlagPropertyGetter, 'i');
  defineGetter(proto, Predefined::global, regExpFlagPropertyGetter, 'g');
  defineGetter(proto, Predefined::unicode, regExpFlagPropertyGetter, 'u');
  defineGetter(proto, Predefined::sticky, regExpFlagPropertyGetter, 'y');
  defineGetter(proto, Predefined::dotAll, regExpFlagPropertyGetter, 's');
  defineGetter(proto, Predefined::hasIndices, regExpFlagPropertyGetter, 'd');

  defineGetter(lv.cons, Predefined::dollar1, regExpDollarNumberGetter, 1);
  defineGetter(lv.cons, Predefined::dollar2, regExpDollarNumberGetter, 2);
  defineGetter(lv.cons, Predefined::dollar3, regExpDollarNumberGetter, 3);
  defineGetter(lv.cons, Predefined::dollar4, regExpDollarNumberGetter, 4);
  defineGetter(lv.cons, Predefined::dollar5, regExpDollarNumberGetter, 5);
  defineGetter(lv.cons, Predefined::dollar6, regExpDollarNumberGetter, 6);
  defineGetter(lv.cons, Predefined::dollar7, regExpDollarNumberGetter, 7);
  defineGetter(lv.cons, Predefined::dollar8, regExpDollarNumberGetter, 8);
  defineGetter(lv.cons, Predefined::dollar9, regExpDollarNumberGetter, 9);
  defineGetter(lv.cons, Predefined::leftContext, regExpLeftContextGetter);
  defineGetter(lv.cons, Predefined::dollarBacktick, regExpLeftContextGetter);
  defineGetter(lv.cons, Predefined::rightContext, regExpRightContextGetter);
  defineGetter(lv.cons, Predefined::dollarApostrophe, regExpRightContextGetter);
  defineGetter(lv.cons, Predefined::dollarUnderscore, regExpInputGetter);
  defineGetter(lv.cons, Predefined::input, regExpInputGetter);
  defineGetter(lv.cons, Predefined::dollarAmpersand, regExpLastMatchGetter);
  defineGetter(lv.cons, Predefined::lastMatch, regExpLastMatchGetter);
  defineGetter(lv.cons, Predefined::dollarPlus, regExpLastParenGetter);
  defineGetter(lv.cons, Predefined::lastParen, regExpLastParenGetter);

  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::toString),
      nullptr,
      regExpPrototypeToString,
      0);
  (void)defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::SymbolMatch),
      Predefined::getSymbolID(Predefined::squareSymbolMatch),
      nullptr,
      regExpPrototypeSymbolMatch,
      1,
      dpf);
  (void)defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::SymbolSearch),
      Predefined::getSymbolID(Predefined::squareSymbolSearch),
      nullptr,
      regExpPrototypeSymbolSearch,
      1,
      dpf);
  (void)defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::SymbolReplace),
      Predefined::getSymbolID(Predefined::squareSymbolReplace),
      nullptr,
      regExpPrototypeSymbolReplace,
      2,
      dpf);
  (void)defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::SymbolSplit),
      Predefined::getSymbolID(Predefined::squareSymbolSplit),
      nullptr,
      regExpPrototypeSymbolSplit,
      2,
      dpf);

  defineGetter(proto, Predefined::flags, regExpFlagsGetter);

  return lv.cons.getHermesValue();
}

/// \return true if \p cp is a RegExp SyntaxCharacter:
/// one of ^ $ \ . * + ? ( ) [ ] { } |.
static bool isRegExpSyntaxChar(uint32_t cp) {
  switch (cp) {
    case u'^':
    case u'$':
    case u'\\':
    case u'.':
    case u'*':
    case u'+':
    case u'?':
    case u'(':
    case u')':
    case u'[':
    case u']':
    case u'{':
    case u'}':
    case u'|':
      return true;
    default:
      return false;
  }
}

/// \return true if \p cp is one of the "other punctuators" that
/// EncodeForRegExpEscape escapes.
static bool isOtherPunctuator(uint32_t cp) {
  switch (cp) {
    case u',':
    case u'-':
    case u'=':
    case u'<':
    case u'>':
    case u'#':
    case u'&':
    case u'!':
    case u'%':
    case u':':
    case u';':
    case u'@':
    case u'~':
    case u'\'':
    case u'`':
    case u'"':
      return true;
    default:
      return false;
  }
}

/// Append "\xNN" (lowercase hex) to \p R for \p c, which must be <= 0xFF.
static void appendHexEscape(llvh::SmallVectorImpl<char16_t> &R, uint32_t c) {
  assert(c <= 0xFF && "appendHexEscape argument out of bounds");
  R.append(
      {u'\\',
       u'x',
       static_cast<char16_t>(
           llvh::hexdigit((c >> 4) & 0xf, /* LowerCase */ true)),
       static_cast<char16_t>(llvh::hexdigit(c & 0xf, /* LowerCase */ true))});
}

/// Append "\uNNNN" (lowercase hex) to \p R for code unit \p cu.
static void appendUnicodeEscape(
    llvh::SmallVectorImpl<char16_t> &R,
    char16_t cu) {
  R.append(
      {u'\\',
       u'u',
       static_cast<char16_t>(
           llvh::hexdigit((cu >> 12) & 0xf, /* LowerCase */ true)),
       static_cast<char16_t>(
           llvh::hexdigit((cu >> 8) & 0xf, /* LowerCase */ true)),
       static_cast<char16_t>(
           llvh::hexdigit((cu >> 4) & 0xf, /* LowerCase */ true)),
       static_cast<char16_t>(llvh::hexdigit(cu & 0xf, /* LowerCase */ true))});
}

/// ES2026 22.2.5.1.1 EncodeForRegExpEscape ( c )
/// Append the encoding of code point \p cp to \p R.
static void encodeForRegExpEscape(
    llvh::SmallVectorImpl<char16_t> &R,
    uint32_t cp) {
  // 1. If cp is matched by SyntaxCharacter or cp is U+002F (SOLIDUS), then
  if (cp == u'/' || isRegExpSyntaxChar(cp)) {
    // a. Return the string-concatenation of 0x005C (REVERSE SOLIDUS) and
    // UTF16EncodeCodePoint(cp)
    R.push_back(u'\\');
    R.push_back(static_cast<char16_t>(cp));
    return;
  }
  // 2. If cp is a code point listed in the “Code Point” column of Table 62,
  // then
  switch (cp) {
    // a. Return the string-concatenation of 0x005C (REVERSE SOLIDUS) and the
    // string in the “ControlEscape” column of the row whose “Code Point” column
    // contains cp.
    case 0x09:
      R.push_back(u'\\');
      R.push_back(u't');
      return;
    case 0x0A:
      R.push_back(u'\\');
      R.push_back(u'n');
      return;
    case 0x0B:
      R.push_back(u'\\');
      R.push_back(u'v');
      return;
    case 0x0C:
      R.push_back(u'\\');
      R.push_back(u'f');
      return;
    case 0x0D:
      R.push_back(u'\\');
      R.push_back(u'r');
      return;
    default:
      break;
  }
  // 3. Let otherPunctuators be the string-concatenation of ",-=<>#&!%:;@~'`"
  // and the code unit 0x0022 (QUOTATION MARK).
  // 4. Let toEscape be StringToCodePoints(otherPunctuators).
  bool needsEscape = isOtherPunctuator(cp);
  if (!needsEscape && cp <= 0xFFFF) {
    char16_t c16 = static_cast<char16_t>(cp);
    needsEscape = isWhiteSpaceChar(c16) || isLineTerminatorChar(c16) ||
        isHighSurrogate(cp) || isLowSurrogate(cp);
  }
  // 5. If toEscape contains cp, cp is matched by either WhiteSpace or
  // LineTerminator, or cp has the same numeric value as a leading surrogate or
  // trailing surrogate, then
  if (needsEscape) {
    // a. Let cpNum be the numeric value of cp.
    // b. If cpNum ≤ 0xFF, then
    if (cp <= 0xFF) {
      // i. Let hex be Number::toString((cpNum), 16).
      // ii. Return the string-concatenation of the code unit 0x005C (REVERSE
      // SOLIDUS), "x", and StringPad(hex, 2, "0", start).
      appendHexEscape(R, cp);
      return;
    }
    // c. Let escaped be the empty String.
    // d. Let codeUnits be UTF16EncodeCodePoint(cp).
    // e. For each code unit cu of codeUnits, do
    // i. Set escaped to the string-concatenation of escaped and
    // UnicodeEscape(cu).
    // f. Return escaped.
    appendUnicodeEscape(R, static_cast<char16_t>(cp));
    return;
  }
  // 6. Return UTF16EncodeCodePoint(cp).
  utf16Encoding(cp, R);
}

/// ES2026 22.2.5.1 RegExp.escape ( S )
CallResult<HermesValue> regExpEscape(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. If S is not a String, throw a TypeError exception.
  if (LLVM_UNLIKELY(!args.getArg(0).isString())) {
    return runtime.raiseTypeError("RegExp.escape() argument must be a string");
  }

  struct : public Locals {
    PinnedValue<StringPrimitive> string;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.string.castAndSetHermesValue<StringPrimitive>(args.getArg(0));

  // 2. Let escaped be the empty String.
  uint32_t len = lv.string->getStringLength();
  SmallU16String<16> escaped{};
  escaped.reserve(len);

  auto view = StringPrimitive::createStringView(runtime, lv.string);
  // 3. Let cpList be StringToCodePoints(S).
  // 4. For each code point c of cpList, do
  for (uint32_t i = 0; i < len;) {
    char16_t first = view[i];
    uint32_t cp = first;
    if (isHighSurrogate(first) && i + 1 < len && isLowSurrogate(view[i + 1])) {
      cp = utf16SurrogatePairToCodePoint(first, view[i + 1]);
      i += 2;
    } else {
      ++i;
    }
    // 4a. If escaped is the empty String and c is matched by DecimalDigit or
    // AsciiLetter, escape the leading character as "\xNN". The hex of an ASCII
    // digit or letter is always exactly 2 digits.
    if (escaped.empty() && cp < 128 && llvh::isAlnum(static_cast<char>(cp))) {
      appendHexEscape(escaped, cp);
    } else {
      // 4b. Else, append EncodeForRegExpEscape(c).
      encodeForRegExpEscape(escaped, cp);
    }
  }

  // 5. Return escaped.
  return StringPrimitive::create(runtime, escaped);
}

/// ES2022 22.2.5.2.4 GetStringIndex ( S, e )
static uint32_t getStringIndex(Handle<StringPrimitive> S, uint32_t e) {
  // Relevant spec: Let eUTF be the smallest index into S that corresponds to
  // the character at element e of codepoints. If e is greater than or equal to
  // the number of elements in codepoints, then eUTF is the number of code units
  // in S.
  // This is a longwinded way of saying that we don't set e to match the
  // trailing member of a surrogate pair.
  if (e > 0 && e < S->getStringLength()) {
    if (isHighSurrogate(S->at(e - 1)) && isLowSurrogate(S->at(e))) {
      e -= 1;
    }
  }
  return e;
}

/// E2024 22.2.3.2 Runtime Semantics: RegExpAlloc ( newTarget )
static CallResult<HermesValue> regExpAlloc(
    Runtime &runtime,
    Handle<Callable> newTarget) {
  if (LLVM_LIKELY(
          newTarget.getHermesValue().getRaw() ==
          runtime.regExpConstructor.getHermesValue().getRaw())) {
    return JSRegExp::create(runtime).getHermesValue();
  }
  struct : public Locals {
    PinnedValue<JSObject> selfParent;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  CallResult<PseudoHandle<JSObject>> thisParentRes =
      NativeConstructor::parentForNewThis_RJS(
          runtime,
          Handle<Callable>::vmcast(newTarget),
          runtime.regExpPrototype);
  if (LLVM_UNLIKELY(thisParentRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.selfParent = std::move(*thisParentRes);
  return JSRegExp::create(runtime, lv.selfParent).getHermesValue();
}

/// ES11 21.2.3.2.2 RegExpInitialize ( obj, pattern, flags ) 1-4
static ExecutionStatus regExpInitialize(
    Runtime &runtime,
    Handle<> obj,
    Handle<> pattern,
    Handle<> flags,
    MutableHandle<JSRegExp> regExpOut) {
  auto regExpObj = Handle<JSRegExp>::dyn_vmcast(obj);
  if (LLVM_UNLIKELY(!regExpObj)) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<StringPrimitive> P;
    PinnedValue<StringPrimitive> F;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 1. If pattern is undefined, let P be the empty String.
  // 2. Else, let P be ? ToString(pattern).
  lv.P = runtime.getPredefinedString(Predefined::emptyString);
  if (!pattern->isUndefined()) {
    auto strRes = toString_RJS(runtime, pattern);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.P = strRes->get();
  }
  // 3. If flags is undefined, let F be the empty String.
  // 4. Else, let F be ? ToString(flags).
  lv.F = runtime.getPredefinedString(Predefined::emptyString);
  if (!flags->isUndefined()) {
    auto strRes = toString_RJS(runtime, flags);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.F = strRes->get();
  }
  if (JSRegExp::initialize(regExpObj, runtime, lv.P, lv.F) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  regExpOut.castAndSetHermesValue<JSRegExp>(regExpObj.getHermesValue());
  return ExecutionStatus::RETURNED;
}

/// ES6.0 21.2.3.2.3 Runtime Semantics: RegExpCreate ( P, F )
ExecutionStatus regExpCreate(
    Runtime &runtime,
    Handle<> P,
    Handle<> F,
    MutableHandle<JSRegExp> regExpOut) {
  auto objRes = regExpAlloc(runtime, runtime.regExpConstructor);
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  struct : public Locals {
    PinnedValue<> obj;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.obj = objRes.getValue();
  return regExpInitialize(runtime, lv.obj, P, F, regExpOut);
}

/// ES2024 22.2.4.1 RegExp ( pattern, flags )
/// The internals of \c regExpConstructor. It's extracted to allow internal call
/// sites such as @@MatchAll to construct a new JSRegExp via handles.
static ExecutionStatus regExpConstructorInternal(
    Runtime &runtime,
    Handle<> pattern,
    Handle<> flags,
    Handle<> NewTarget,
    MutableHandle<JSObject> resultOut) {
  struct : public Locals {
    PinnedValue<> P;
    PinnedValue<> F;
    PinnedValue<JSRegExp> regExpResult;
    PinnedValue<JSObject> objHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 1. Let patternIsRegExp be IsRegExp(pattern).
  auto callRes = isRegExp(runtime, pattern);
  if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<Callable> newTarget = NewTarget->isUndefined()
      ? runtime.regExpConstructor
      : Handle<Callable>::vmcast(NewTarget);
  bool patternIsRegExp = callRes.getValue();
  // 2. If NewTarget is undefined, then
  if (NewTarget->isUndefined()) {
    // a. Let newTarget be the active function object. (set above)
    // b. If patternIsRegExp is true and flags is undefined, then
    if (patternIsRegExp && flags->isUndefined()) {
      // i. Let patternConstructor be ? Get(pattern, "constructor").
      auto patternObj = Handle<JSObject>::vmcast(pattern);
      auto patternConstructor = JSObject::getNamed_RJS(
          patternObj,
          runtime,
          Predefined::getSymbolID(Predefined::constructor));
      if (LLVM_UNLIKELY(patternConstructor == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      // ii. If SameValue(newTarget, patternConstructor) is true, return
      // pattern.
      if (isSameValue(newTarget.getHermesValue(), patternConstructor->get())) {
        // Note: unfortunately, even at this point it's still unsafe to assume
        // pattern is a JSRegExp, see the objWithRegExpCons from
        // test/hermes/regexp.js for an example of how such assumptions falls.
        resultOut.castAndSetHermesValue<JSObject>(pattern.getHermesValue());
        return ExecutionStatus::RETURNED;
      }
    }
  }

  // 3. Else, a. Let newTarget be NewTarget. (set above)

  // 4. If pattern is an Object and pattern has a [[RegExpMatcher]] internal
  // slot, then
  if (auto patternAsRegExp = Handle<JSRegExp>::dyn_vmcast(pattern)) {
    // a. Let P be the value of pattern’s [[OriginalSource]] internal slot.
    lv.P =
        JSRegExp::getPattern(patternAsRegExp.get(), runtime).getHermesValue();
    // b. If flags is undefined, let F be the value of pattern’s
    // [[OriginalFlags]] internal slot.
    if (flags->isUndefined()) {
      auto flagsStr =
          JSRegExp::getSyntaxFlags(patternAsRegExp.get()).toString();
      auto strRes = StringPrimitive::create(runtime, flagsStr);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.F = strRes.getValue();
    } else {
      // c. Else, let F be flags.
      lv.F = flags.getHermesValue();
    }
  } else if (patternIsRegExp) {
    // 5. Else if patternIsRegExp is true, then
    //   a. Let P be ? Get(pattern, "source").
    Handle<JSObject> patternObj = Handle<JSObject>::vmcast(pattern);
    auto propRes = JSObject::getNamed_RJS(
        patternObj, runtime, Predefined::getSymbolID(Predefined::source));
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.P = std::move(propRes.getValue());
    //   b. If flags is undefined, then
    if (flags->isUndefined()) {
      //   i. Let F be ? Get(pattern, "flags").
      propRes = JSObject::getNamed_RJS(
          patternObj, runtime, Predefined::getSymbolID(Predefined::flags));
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.F = std::move(propRes.getValue());
    } else {
      //   c. Else, let F be flags.
      lv.F = flags.getHermesValue();
    }
  } else {
    // 6. Else,
    //   a. Let P be pattern.
    lv.P = pattern.getHermesValue();
    //   b. Let F be flags.
    lv.F = flags.getHermesValue();
  }
  // 7. Let O be ? RegExpAlloc(newTarget).
  auto objRes = regExpAlloc(runtime, newTarget);
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 8. Return ? RegExpInitialize(O, P, F).
  lv.objHandle.castAndSetHermesValue<JSObject>(objRes.getValue());
  auto status = regExpInitialize(
      runtime,
      lv.objHandle,
      lv.P,
      lv.F,
      MutableHandle<JSRegExp>::aliasForOutput(resultOut));
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return ExecutionStatus::RETURNED;
}

/// ES6 21.2.3.1 RegExp ( pattern, flags )
// This is just a wrapper of \c regExpConstructorInternal to take NativeArgs.
CallResult<HermesValue> regExpConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  struct : public Locals {
    PinnedValue<JSObject> regExpRes;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  Handle<> pattern = args.getArgHandle(0);
  Handle<> flags = args.getArgHandle(1);
  auto status = regExpConstructorInternal(
      runtime,
      pattern,
      flags,
      args.getNewTargetHandle(),
      MutableHandle<JSObject>{lv.regExpRes});
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return lv.regExpRes.getHermesValue();
}

/// Wrapper for regExpConstructorInternal to implement a fast path for
/// Construct(%RegExp%, « pattern, flags ») and avoid a recompilation in the
/// common case where \p pattern is a JSRegExp with [[OriginalFlags]] equal to
/// \p flags.
static ExecutionStatus regExpConstructorFastCopy(
    Runtime &runtime,
    Handle<> pattern,
    Handle<StringPrimitive> flags,
    MutableHandle<JSRegExp> regExpOut) {
  if (auto R = Handle<JSRegExp>::dyn_vmcast(pattern)) {
    regExpOut = JSRegExp::create(runtime);
    if (LLVM_UNLIKELY(
            JSRegExp::initialize(regExpOut, runtime, R, flags) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return ExecutionStatus::RETURNED;
  }
  auto newRegexpRes = regExpConstructorInternal(
      runtime,
      pattern,
      flags,
      runtime.regExpConstructor,
      MutableHandle<JSObject>::aliasForOutput(
          regExpOut.unsafeGetPinnedHermesValue()));
  if (LLVM_UNLIKELY(newRegexpRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  assert(
      vmisa<JSRegExp>(regExpOut.getHermesValue()) &&
      "regExpConstructorInternal should return a JSRegExp");
  return ExecutionStatus::RETURNED;
}

/// Build an object whose property keys are identical to \p mappingObj. Each
/// value for a given property key `k` in this object is
/// `getCapture(mappingObj[k])`: \p getCapture maps an index of the match
/// (0 is the whole match, n the n-th group) to that element as a
/// SmallHermesValue, and must not allocate. The result has a null
/// prototype.
template <typename GetCapture>
static CallResult<PseudoHandle<JSObject>> buildGroupsObject(
    Runtime &runtime,
    Handle<JSObject> mappingObj,
    const GetCapture &getCapture) {
  struct : public Locals {
    PinnedValue<HiddenClass> clazz;
    PinnedValue<JSObject> groupsObj;
    PinnedValue<> tmpVal;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.clazz = mappingObj->getClass(runtime);

  if (LLVM_LIKELY(!lv.clazz->isDictionary())) {
    // Fast path: non-dictionary class is safe to share between groupsObj and
    // mappingObj.
    lv.groupsObj =
        JSObject::create(runtime, Runtime::makeNullHandle<JSObject>(), lv.clazz)
            .get();
    HiddenClass::forEachProperty(
        lv.clazz, runtime, [&](SymbolID, NamedPropertyDescriptor desc) {
          assert(
              !desc.flags.privateName &&
              "private name not expected in regex mapping object");
          auto idx = static_cast<uint32_t>(
              JSObject::getNamedSlotValueUnsafe(*mappingObj, runtime, desc.slot)
                  .getNumber(runtime));
          JSObject::setNamedSlotValueUnsafe(
              lv.groupsObj.get(), runtime, desc.slot, getCapture(idx));
        });
    return PseudoHandle<JSObject>::create(lv.groupsObj.get());
  }

  // Slow path: mappingObj's class is in dictionary mode. Dictionary classes
  // are owned by a single object and mutated in place; sharing one with
  // groupsObj would let a later mutation on this `groups` grow the shared
  // class past the underlying propStorage of any sibling (out-of-bounds
  // read/write). Build groupsObj with its own fresh class instead.
  //
  // Calling defineNewOwnProperty inside HiddenClass::forEachPropertyWhile is
  // safe: it only touches groupsObj's class chain (independent from
  // lv.clazz), and forEachPropertyWhile flushes the GCScope after each
  // callback so handle counts stay bounded.
  lv.groupsObj =
      JSObject::create(runtime, Runtime::makeNullHandle<JSObject>()).get();
  ExecutionStatus status = ExecutionStatus::RETURNED;
  HiddenClass::forEachPropertyWhile(
      lv.clazz,
      runtime,
      [&](Runtime &, SymbolID id, NamedPropertyDescriptor desc) -> bool {
        auto idx = static_cast<uint32_t>(
            JSObject::getNamedSlotValueUnsafe(*mappingObj, runtime, desc.slot)
                .getNumber(runtime));
        lv.tmpVal = getCapture(idx).unboxToHV(runtime);
        if (LLVM_UNLIKELY(
                JSObject::defineNewOwnProperty(
                    lv.groupsObj,
                    runtime,
                    id,
                    PropertyFlags::defaultNewNamedPropertyFlags(),
                    lv.tmpVal) == ExecutionStatus::EXCEPTION)) {
          status = ExecutionStatus::EXCEPTION;
          return false;
        }
        return true;
      });
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return PseudoHandle<JSObject>::create(lv.groupsObj.get());
}

static ExecutionStatus setMatchGroups(
    Runtime &runtime,
    Handle<JSArray> matchObj,
    Handle<JSObject> mappingObj) {
  // matchObj is created with a HiddenClass that already has the groups
  // property.
  NamedPropertyDescriptor groupsDesc;
  auto pos = JSObject::getOwnNamedDescriptor(
      matchObj,
      runtime,
      Predefined::getSymbolID(Predefined::groups),
      groupsDesc);
  assert(pos && "match object is missing .groups property");
  (void)pos;

  // If there are no capture groups, then set groups to undefined.
  if (!mappingObj) {
    auto shv = SmallHermesValue::encodeUndefinedValue();
    JSObject::setNamedSlotValueUnsafe(matchObj.get(), runtime, groupsDesc, shv);
    return ExecutionStatus::RETURNED;
  }

  // The `__proto__` property on the groups object is not special,
  // and does not affect the [[Prototype]] of the resulting groups object.
  // This means that the prototype of the resulting groups object is null.
  auto groupsRes = buildGroupsObject(
      runtime, mappingObj, [&](uint32_t i) -> SmallHermesValue {
        return matchObj->at(runtime, i);
      });
  if (LLVM_UNLIKELY(groupsRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;

  auto shv = SmallHermesValue::encodeObjectValue(groupsRes->get(), runtime);
  JSObject::setNamedSlotValueUnsafe(matchObj.get(), runtime, groupsDesc, shv);
  return ExecutionStatus::RETURNED;
}

/// ES2022 22.2.7.8 MakeMatchIndicesIndexPairArray
static ExecutionStatus makeMatchIndicesIndexPairArray(
    Runtime &runtime,
    Handle<StringPrimitive> S,
    RegExpMatch indices,
    Handle<JSObject> mappingObj,
    bool hasGroups,
    MutableHandle<JSArray> resultOut) {
  struct : public Locals {
    PinnedValue<> matchIndexPair;
    PinnedValue<> groups;
    PinnedValue<JSArray> A;
    PinnedValue<JSArray> pair;
    PinnedValue<JSObject> groupsObj;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 1. Let n be the number of elements in indices.
  size_t n = indices.size();
  // 2-4 skipped because mappingObj is not list, but an object in our
  // implementation.
  // 5. Let A be ! ArrayCreate(n).
  auto arrRes = JSArray::create(runtime, n, n);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.A = std::move(*arrRes);
  JSArray::setStorageEndIndex(lv.A, runtime, indices.size());

  // 6-8. done later. We can't code exactly to spec here because mappingObj
  // (roughly groupNames in the spec) is not an array, but an object containing
  // group name and index.

  // 9. For each integer i such that 0 ≤ i < n, in ascending order, do
  for (size_t i = 0; i < n; i++) {
    // a. Let matchIndices be indices[i]
    auto matchIndices = indices[i];
    // b. If matchIndices is not undefined, then
    if (matchIndices) {
      // i. Let matchIndexPair be GetMatchIndexPair(S, matchIndices).
      auto matchIndexPairRes = JSArray::create(runtime, 2, 2);
      if (LLVM_UNLIKELY(matchIndexPairRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.pair = std::move(*matchIndexPairRes);
      JSArray::setStorageEndIndex(lv.pair, runtime, 2);
      auto firstIdx =
          SmallHermesValue::encodeNumberValue(matchIndices->location, runtime);
      JSArray::unsafeSetExistingElementAt(lv.pair.get(), runtime, 0, firstIdx);
      auto secondIdx = SmallHermesValue::encodeNumberValue(
          matchIndices->location + matchIndices->length, runtime);
      JSArray::unsafeSetExistingElementAt(lv.pair.get(), runtime, 1, secondIdx);
      lv.matchIndexPair = lv.pair.getHermesValue();
    } else {
      // c. Else,
      // i. Let matchIndexPair be undefined.
      lv.matchIndexPair = Runtime::getUndefinedValue();
    }
    // d. Perform ! CreateDataPropertyOrThrow(A, ! ToString(𝔽(i)),
    // matchIndexPair).
    auto shv = SmallHermesValue::encodeHermesValue(
        lv.matchIndexPair.getHermesValue(), runtime);
    JSArray::unsafeSetExistingElementAt(lv.A.get(), runtime, i, shv);
  }

  // This is done out of order. See note above.
  // 6. If hasGroups is true, then
  if (hasGroups) {
    // a. Let groups be OrdinaryObjectCreate(null).
    // 9.e. If i > 0 and groupNames[i - 1] is not undefined, then
    // ii. Perform ! CreateDataPropertyOrThrow(groups, groupNames[i-1],
    // matchIndexPair).
    auto groupsRes = buildGroupsObject(
        runtime, mappingObj, [&](uint32_t i) -> SmallHermesValue {
          return lv.A->at(runtime, i);
        });
    if (LLVM_UNLIKELY(groupsRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    lv.groupsObj = groupsRes->get();
    lv.groups = lv.groupsObj.getHermesValue();
  } else {
    // 7. Else,
    // a. Let groups be undefined.
    lv.groups = Runtime::getUndefinedValue();
  }

  // 8. Perform ! CreateDataPropertyOrThrow(A, "groups", groups).
  if (LLVM_UNLIKELY(
          JSObject::defineOwnProperty(
              lv.A,
              runtime,
              Predefined::getSymbolID(Predefined::groups),
              DefinePropertyFlags::getDefaultNewPropertyFlags(),
              lv.groups) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 10. Return A.
  resultOut.castAndSetHermesValue<JSArray>(lv.A.getHermesValue());
  return ExecutionStatus::RETURNED;
}

/// ES6 21.2.5.2.2 RegExpBuiltinExec ( R, S ) up to, but not including,
/// building the result array: the Get and ToLength of lastIndex, the
/// search, the Set of lastIndex, and the unicode adjustment of the ranges.
/// \return false on no match, leaving \p match empty; on a match, set
/// \p match to its ranges (element 0 is the whole match) and \return true.
static CallResult<bool> regExpBuiltinExecMatch(
    Handle<JSRegExp> regexp,
    Runtime &runtime,
    Handle<StringPrimitive> S,
    RegExpMatch &match) {
  struct : public Locals {
    PinnedValue<> propHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  GCScope gcScope{runtime};

  // Let length be the number of code units in S.
  const uint32_t length = S->getStringLength();

  // Let lastIndex be ? ToLength(? Get(R, "lastIndex")).
  auto propRes = runtime.getNamed(regexp, PropCacheID::RegExpLastIndex);
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.propHandle = std::move(*propRes);
  auto lengthRes = toLengthU64(runtime, lv.propHandle);
  if (LLVM_UNLIKELY(lengthRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t lastIndex = *lengthRes;

  // Let flags be R.[[OriginalFlags]]
  const regex::SyntaxFlags flags = JSRegExp::getSyntaxFlags(regexp.get());

  // If flags contains "g", let global be true, else let global be false
  // If flags contains "y", let sticky be true, else let sticky be false.
  // If flags contains "u", let fullUnicode be true, else let fullUnicode be
  // false.
  const bool global = flags.global;
  const bool sticky = flags.sticky;
  const bool fullUnicode = flags.unicode;

  // If global is false and sticky is false, set lastIndex to 0.
  if (!global && !sticky) {
    lastIndex = 0;
  }

  // The ES6 spec is cast in terms of a loop on "matchSucceeded". This is
  // handled internally by JSRegExp::search for efficiency reasons.
  // (Note that lastIndex == length is allowed: it may match an empty range at
  // the end of the string.)
  match.clear();
  if (lastIndex <= length) {
    assert(
        lastIndex <= UINT32_MAX &&
        "lastIndex should fit in u32 because it is <= a string length");
    uint32_t searchStartOffset = static_cast<uint32_t>(lastIndex);
    auto matchResult = JSRegExp::search(regexp, runtime, S, searchStartOffset);
    if (LLVM_UNLIKELY(matchResult == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    match = std::move(*matchResult);
  }

  if (match.empty()) {
    // No match. The following implements both:
    //   If lastIndex > length, then
    //     If global is true or sticky is true, then
    //         Let setStatus be Set(R, "lastIndex", 0, true);
    //         ReturnIfAbrupt(setStatus)
    //     return null
    // and also:
    //   If r is failure, then
    //     If sticky is true, then
    //       Let setStatus be Set(R, "lastIndex", 0, true);
    //       ReturnIfAbrupt(setStatus)
    //     Return null
    if (global || sticky) {
      if (LLVM_UNLIKELY(
              setLastIndex(regexp, runtime, 0) == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    }
    return false;
  }

  // We have a match!
  // If using unicode, ensure that no indices can be set to a trailing
  // surrogate.
  if (fullUnicode) {
    for (auto &mg : match) {
      if (!mg)
        continue;
      auto captureStart = getStringIndex(S, mg->location);
      auto captureEnd = getStringIndex(S, mg->location + mg->length);
      mg = RegExpMatchRange{captureStart, captureEnd - captureStart};
    }
  }

  // "If global is true or sticky is true, Let setStatus be Set(R, "lastIndex",
  // e, true)."
  // Here 'e' is the end of the total match.
  if (global || sticky) {
    auto totalMatch = *match.front();
    uint32_t e = totalMatch.location + totalMatch.length;
    if (LLVM_UNLIKELY(
            setLastIndex(regexp, runtime, e) == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return true;
}

// ES6 21.2.5.2.2
ExecutionStatus directRegExpExec(
    Handle<JSRegExp> regexp,
    Runtime &runtime,
    Handle<StringPrimitive> S,
    MutableHandle<JSArray> resultOut) {
  struct : public Locals {
    PinnedValue<JSArray> A;
    PinnedValue<StringPrimitive> strHandle;
    PinnedValue<JSArray> indicesArray;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  GCScope gcScope{runtime};

  RegExpMatch match;
  auto matchRes = regExpBuiltinExecMatch(regexp, runtime, S, match);
  if (LLVM_UNLIKELY(matchRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  if (!*matchRes) {
    resultOut.clear();
    return ExecutionStatus::RETURNED;
  }

  // If flags contains "d", let hasIndices be true, else let hasIndices be
  // false.
  const bool hasIndices = JSRegExp::getSyntaxFlags(regexp.get()).hasIndices;

  auto arrRes = JSArray::createAndAllocPropStorage(
      runtime,
      runtime.arrayPrototype,
      runtime.regExpMatchClass,
      match.size(),
      match.size());
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.A = std::move(*arrRes);

  // A already has .index and .groups.
  NamedPropertyDescriptor indexDesc;
  bool res = JSObject::getOwnNamedDescriptor(
      lv.A, runtime, Predefined::getSymbolID(Predefined::index), indexDesc);
  assert(res && "match object is missing .index property");
  (void)res;
  auto indexSHV =
      SmallHermesValue::encodeNumberValue(match.front()->location, runtime);
  JSObject::setNamedSlotValueUnsafe(lv.A.get(), runtime, indexDesc, indexSHV);

  NamedPropertyDescriptor inputDesc;
  res = JSObject::getOwnNamedDescriptor(
      lv.A, runtime, Predefined::getSymbolID(Predefined::input), inputDesc);
  assert(res && "match object is missing .input property");
  (void)res;
  auto inputSHV = SmallHermesValue::encodeStringValue(*S, runtime);
  JSObject::setNamedSlotValueUnsafe(lv.A.get(), runtime, inputDesc, inputSHV);

  // If R contains any GroupName, then let hasGroups be true.
  Handle<JSObject> groupNames = regexp->getGroupNameMappings(runtime);
  bool hasGroups = (bool)groupNames;

  // Set capture groups (including the initial full match)
  size_t idx = 0;
  auto marker = gcScope.createMarker();
  for (const auto &mg : match) {
    gcScope.flushToMarker(marker);
    if (!mg) {
      // Match groups that did not match anything become undefined.
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(
                  lv.A, runtime, idx, Runtime::getUndefinedValue()) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
    } else {
      assert(
          mg->location + mg->length >= mg->location &&
          mg->location + mg->length <= S->getStringLength() &&
          "Capture group range unexpectedly out of bounds");
      auto strRes =
          StringPrimitive::slice(runtime, S, mg->location, mg->length);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.strHandle.castAndSetHermesValue<StringPrimitive>(*strRes);
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(lv.A, runtime, idx, lv.strHandle) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
    }
    idx++;
  }

  // If hasIndices is true, then
  if (hasIndices) {
    // Let indicesArray be MakeMatchIndicesIndexPairArray(S, indices,
    // groupNames, hasGroups).
    auto indicesStatus = makeMatchIndicesIndexPairArray(
        runtime,
        S,
        match,
        groupNames,
        hasGroups,
        MutableHandle<JSArray>{lv.indicesArray});
    if (LLVM_UNLIKELY(indicesStatus == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // Perform ! CreateDataPropertyOrThrow(A, "indices", indicesArray).
    if (LLVM_UNLIKELY(
            JSObject::defineOwnProperty(
                lv.A,
                runtime,
                Predefined::getSymbolID(Predefined::indices),
                DefinePropertyFlags::getDefaultNewPropertyFlags(),
                lv.indicesArray) == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }

  if (LLVM_UNLIKELY(
          setMatchGroups(runtime, lv.A, groupNames) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  resultOut.castAndSetHermesValue<JSArray>(lv.A.getHermesValue());
  return ExecutionStatus::RETURNED;
}

/// ES9.0 21.2.5.2.2 Runtime Semantics: RegExpBuiltinExec ( R, S )
/// TODO: this is a wrapper around \f directRegExpExec right now, and it is
/// semantically equivalent to the spec definition without unicode support.
/// After updating this to exactly match the spec, replace usages of \f
/// directRegExpExec with this function.
static CallResult<HermesValue> regExpBuiltinExec(
    Runtime &runtime,
    Handle<JSRegExp> R,
    Handle<StringPrimitive> S) {
  struct : public Locals {
    PinnedValue<JSArray> result;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto status =
      directRegExpExec(R, runtime, S, MutableHandle<JSArray>{lv.result});
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // When the match is empty, result is a nullptr handle
  if (!lv.result.get())
    return HermesValue::encodeNullValue();
  return lv.result.getHermesValue();
}

/// ES6.0 21.2.5.2.3
uint64_t
advanceStringIndex(const StringPrimitive *S, uint64_t index, bool unicode) {
  if (unicode && index + 1 < S->getStringLength() &&
      isHighSurrogate(S->at(index)) && isLowSurrogate(S->at(index + 1))) {
    return index + 2;
  }
  return index + 1;
}

/// Run \p body as the body of a call of the builtin RegExp.prototype.exec
/// with this \p R and the argument \p S: inside a native frame for
/// runtime.regExpPrototypeExec built as Callable::executeCall1,
/// Callable::call and NativeFunction::_nativeCall build it. Stack traces,
/// the debugger and the sampling profiler then see the same exec frame as
/// for a real call. \p body returns CallResult<HermesValue>.
template <typename Body>
static CallResult<HermesValue> runInBuiltinExecFrame(
    Runtime &runtime,
    Handle<JSObject> R,
    Handle<StringPrimitive> S,
    const Body &body) {
  // As Callable::executeCall1.
  ScopedNativeCallFrame newFrame{
      runtime,
      1,
      runtime.regExpPrototypeExec.getHermesValue(),
      HermesValue::encodeUndefinedValue(),
      R.getHermesValue()};
  if (LLVM_UNLIKELY(newFrame.overflowed()))
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  newFrame->getArgRef(0) = S.getHermesValue();
  // As Callable::call. The callee is read again from its root below.
  runtime.potentiallyMoveHeap();
  // As NativeFunction::_nativeCall.
  auto res = NativeFunction::runNativeCallBody(
      *runtime.regExpPrototypeExec, runtime, body);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return res->get();
}

/// ES6.0 21.2.5.2.1 Runtime Semantics: RegExpExec ( R, S )
CallResult<HermesValue>
regExpExec(Runtime &runtime, Handle<JSObject> R, Handle<StringPrimitive> S) {
  struct : public Locals {
    PinnedValue<> exec;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 3. Let exec be Get(R, "exec").
  // 4. ReturnIfAbrupt(exec).
  auto propRes = JSObject::getNamed_RJS(
      R, runtime, Predefined::getSymbolID(Predefined::exec));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.exec = std::move(propRes.getValue());
  // 5. If IsCallable(exec) is true, then
  if (auto execCallable = Handle<Callable>::dyn_vmcast(Handle<>{lv.exec})) {
    // The builtin exec on a JSRegExp would run ToString(S), a no-op on a
    // string, and then RegExpBuiltinExec, whose result is an object or null.
    // Run that in the exec frame without the dispatch through
    // Callable::call.
    if (lv.exec.getHermesValue().getRaw() ==
        runtime.regExpPrototypeExec.getHermesValue().getRaw()) {
      if (auto regExpObj = Handle<JSRegExp>::dyn_vmcast(R)) {
        return runInBuiltinExecFrame(runtime, R, S, [&]() {
          return regExpBuiltinExec(runtime, regExpObj, S);
        });
      }
    }
    // a. Let result be Call(exec, R, «S»).
    // b. ReturnIfAbrupt(result).
    auto callRes =
        Callable::executeCall1(execCallable, runtime, R, S.getHermesValue());
    if (callRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    // c. If Type(result) is neither Object or Null, throw a TypeError
    // exception.
    if (!(*callRes)->isObject() && !(*callRes)->isNull()) {
      return runtime.raiseTypeError(
          "The result of exec can only be object or null.");
    }
    // d. Return result.
    return callRes.toCallResultHermesValue();
  }
  // 6. If R does not have a [[RegExpMatcher]] internal slot, throw a TypeError
  // exception.
  // In our implementation, having a [[RegExpMatcher]] internal slot is
  // equivalent to being a JSRegExp class instance.
  auto regExpObj = Handle<JSRegExp>::dyn_vmcast(R);
  if (LLVM_UNLIKELY(!regExpObj)) {
    return runtime.raiseTypeError(
        "Failed to execute an invalid regular expression object.");
  }
  // 7. Return RegExpBuiltinExec(R, S).
  return regExpBuiltinExec(runtime, regExpObj, S);
}

/// Implementation of RegExp.prototype.exec
/// Returns an Array if a match is found, null if no match is found
/// Helper function for regExpPrototypeExec that takes NativeArgs directly
static CallResult<HermesValue> regExpPrototypeExecImpl(
    Runtime &runtime,
    NativeArgs args) {
  struct : public Locals {
    PinnedValue<StringPrimitive> S;
    PinnedValue<JSArray> result;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  Handle<JSRegExp> regexp = args.dyncastThis<JSRegExp>();
  if (!regexp) {
    return runtime.raiseTypeError(
        "RegExp function called on non-RegExp object");
  }

  auto strRes = toString_RJS(runtime, args.getArgHandle(0));
  if (strRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);
  auto status = directRegExpExec(
      regexp, runtime, lv.S, MutableHandle<JSArray>{lv.result});
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // When the match is empty, result is a nullptr
  if (!lv.result.get())
    return HermesValue::encodeNullValue();
  return lv.result.getHermesValue();
}

CallResult<HermesValue> regExpPrototypeExec(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return regExpPrototypeExecImpl(runtime, args);
}

/// Implementation of RegExp.prototype.test
/// Returns true if a match is found, false otherwise. It runs the effects
/// of the builtin exec (lastIndex, the statics) without building the
/// result array. As before, it never looks up exec.
CallResult<HermesValue> regExpPrototypeTest(void *context, Runtime &runtime) {
  struct : public Locals {
    PinnedValue<StringPrimitive> S;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  Handle<JSRegExp> regexp = args.dyncastThis<JSRegExp>();
  if (!regexp) {
    return runtime.raiseTypeError(
        "RegExp function called on non-RegExp object");
  }

  auto strRes = toString_RJS(runtime, args.getArgHandle(0));
  if (strRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);

  RegExpMatch match;
  auto matchRes = regExpBuiltinExecMatch(regexp, runtime, lv.S, match);
  if (LLVM_UNLIKELY(matchRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return HermesValue::encodeBoolValue(*matchRes);
}

/// Return the ith capture group in the most recent successful RegExp search.
/// If there was no ith capture group, return "".
CallResult<HermesValue> regExpDollarNumberGetter(void *ctx, Runtime &runtime) {
  size_t i = reinterpret_cast<size_t>(ctx);

  auto match = runtime.regExpLastMatch;
  if (match.size() >= i + 1 && *runtime.regExpLastInput) {
    const auto &cap = match[i];
    if (cap.hasValue()) {
      auto strRes = StringPrimitive::slice(
          runtime, runtime.regExpLastInput, cap->location, cap->length);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      return *strRes;
    }
  }

  return HermesValue::encodeStringValue(
      runtime.getPredefinedString(Predefined::emptyString));
}

// ES8 21.2.5.10
CallResult<HermesValue> regExpSourceGetter(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // "If Type(R) is not Object, throw a TypeError exception"
  if (!args.dyncastThis<JSObject>()) {
    return runtime.raiseTypeError(
        "RegExp.prototype.source getter called on non-RegExp");
  }

  Handle<JSRegExp> R = args.dyncastThis<JSRegExp>();
  if (!R) {
    // "If SameValue(R, %RegExpPrototype%) is true, return "(?:)". Otherwise,
    // throw a TypeError exception.
    if (LLVM_UNLIKELY(thisIsRegExpProto(runtime, args))) {
      // Note we don't bother to predefine this string because this should
      // hardly ever be encountered.
      const char *emptyPattern = "(?:)";
      return StringPrimitive::create(
          runtime, ASCIIRef{emptyPattern, strlen(emptyPattern)});
    }
    return runtime.raiseTypeError(
        "RegExp.prototype.source getter called on non-RegExp");
  }

  // "Let src be the value of R’s [[OriginalSource]] internal slot."
  // "Let flags be the value of R’s [[OriginalFlags]] internal slot."
  // "Return EscapeRegExpPattern(src, flags)."
  // Note that ES6 specifies that we provide the flags to EscapeRegExpPattern,
  // however this is only to distinguish a Unicode from a non-Unicode regexp.
  // Beacuse we do not yet support Unicode regexps we can omit the flags.
  struct : public Locals {
    PinnedValue<StringPrimitive> patternHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.patternHandle = JSRegExp::getPattern(R.get(), runtime);
  return JSRegExp::escapePattern(lv.patternHandle, runtime);
}

// ES8 21.2.5.4, 21.2.5.5, 21.2.5.7
CallResult<HermesValue> regExpFlagPropertyGetter(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // Note in ES8, the standard specifies that the RegExp prototype object is not
  // a RegExp but that these accessors check for it as a special case.

  // "Let R be the this value."
  // "If Type(R) is not Object, throw a TypeError exception."
  // "If R does not have an [[OriginalFlags]] internal slot, throw a TypeError
  // exception." (Combine these two checks into a dyncast to JSRegExp).
  Handle<JSRegExp> R = args.dyncastThis<JSRegExp>();
  if (!R) {
    // "If SameValue(R, %RegExpPrototype%) is true, return undefined. Otherwise,
    // throw a TypeError exception"
    if (thisIsRegExpProto(runtime, args)) {
      return HermesValue::encodeUndefinedValue();
    }
    return runtime.raiseTypeError("RegExp getter called on non-RegExp");
  }

  // "Let flags be the value of R’s [[OriginalFlags]] internal slot."
  // "If flags contains the code unit "i/m/g", return true."
  // "Return false."
  auto syntaxFlags = JSRegExp::getSyntaxFlags(R.get());
  switch ((intptr_t)ctx) {
    case 'i':
      return HermesValue::encodeBoolValue(syntaxFlags.ignoreCase);
    case 'm':
      return HermesValue::encodeBoolValue(syntaxFlags.multiline);
    case 'g':
      return HermesValue::encodeBoolValue(syntaxFlags.global);
    case 'u':
      return HermesValue::encodeBoolValue(syntaxFlags.unicode);
    case 'y':
      return HermesValue::encodeBoolValue(syntaxFlags.sticky);
    case 's':
      return HermesValue::encodeBoolValue(syntaxFlags.dotAll);
    case 'd':
      return HermesValue::encodeBoolValue(syntaxFlags.hasIndices);
    default:
      llvm_unreachable("Invalid flag passed to regExpFlagPropertyGetter");
  }
}

CallResult<HermesValue> regExpLeftContextGetter(void *ctx, Runtime &runtime) {
  auto match = runtime.regExpLastMatch;
  if (match.size() >= 1 && *runtime.regExpLastInput) {
    auto strRes = StringPrimitive::slice(
        runtime, runtime.regExpLastInput, 0, match[0]->location);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return *strRes;
  }

  return HermesValue::encodeStringValue(
      runtime.getPredefinedString(Predefined::emptyString));
}

CallResult<HermesValue> regExpRightContextGetter(void *ctx, Runtime &runtime) {
  auto match = runtime.regExpLastMatch;
  if (match.size() >= 1 && *runtime.regExpLastInput) {
    Handle<StringPrimitive> S{runtime.regExpLastInput};
    if (match[0]->location + match[0]->length < S->getStringLength()) {
      auto startIdx = match[0]->location + match[0]->length;
      auto strRes = StringPrimitive::slice(
          runtime, S, startIdx, S->getStringLength() - startIdx);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      return *strRes;
    }
  }

  return HermesValue::encodeStringValue(
      runtime.getPredefinedString(Predefined::emptyString));
}

CallResult<HermesValue> regExpInputGetter(void *ctx, Runtime &runtime) {
  if (*runtime.regExpLastInput) {
    return runtime.regExpLastInput.getHermesValue();
  }

  return HermesValue::encodeStringValue(
      runtime.getPredefinedString(Predefined::emptyString));
}

CallResult<HermesValue> regExpLastMatchGetter(void *ctx, Runtime &runtime) {
  auto match = runtime.regExpLastMatch;
  if (match.size() >= 1 && *runtime.regExpLastInput) {
    auto strRes = StringPrimitive::slice(
        runtime, runtime.regExpLastInput, match[0]->location, match[0]->length);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return *strRes;
  }

  return HermesValue::encodeStringValue(
      runtime.getPredefinedString(Predefined::emptyString));
}

CallResult<HermesValue> regExpLastParenGetter(void *ctx, Runtime &runtime) {
  auto match = runtime.regExpLastMatch;
  if (match.size() >= 2 && *runtime.regExpLastInput) {
    const auto &cap = match.back();
    if (cap.hasValue()) {
      auto strRes = StringPrimitive::slice(
          runtime, runtime.regExpLastInput, cap->location, cap->length);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      return *strRes;
    }
  }

  return HermesValue::encodeStringValue(
      runtime.getPredefinedString(Predefined::emptyString));
}

/// ES6.0 21.1.3.14.1
/// Transforms a replacement string by substituting $ replacement strings.
/// \p captures can be a null pointer.
CallResult<HermesValue> getSubstitution(
    Runtime &runtime,
    Handle<StringPrimitive> matched,
    Handle<StringPrimitive> str,
    uint32_t position,
    Handle<ArrayStorageSmall> captures,
    Handle<JSObject> namedCaptures,
    Handle<StringPrimitive> replacement) {
  struct : public Locals {
    PinnedValue<StringPrimitive> captureStrHandle;
    PinnedValue<> captureValueHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 1. Assert: Type(matched) is String.
  // 2. Let matchLength be the number of code units in matched.
  uint32_t matchLength = matched->getStringLength();
  // 3. Assert: Type(str) is String.
  // 4. Let stringLength be the number of code units in str.
  uint32_t stringLength = str->getStringLength();
  // 5. Assert: position is a nonnegative integer.
  // 6. Assert: position ≤ stringLength.
  assert(
      position <= stringLength &&
      "The matched position should be within the string length.");
  // 7. Assert: captures is a possibly empty List of Strings.
  // 8. Assert: Type(replacement) is String
  // 9. Let tailPos be position + matchLength.
  uint32_t tailPos = position + matchLength;
  // 10. Let m be the number of elements in captures.
  size_t m = captures ? captures->size() : 0;

  // 11. Let result be a String value derived from replacement by copying code
  // unit elements from replacement to result while performing replacements as
  // specified in Table 45. These $ replacements are done left-to- right, and,
  // once such a replacement is performed, the new replacement text is not
  // subject to further replacements.
  auto replacementView =
      StringPrimitive::createStringView(runtime, replacement);
  auto stringView = StringPrimitive::createStringView(runtime, str);
  auto matchedStrView = StringPrimitive::createStringView(runtime, matched);
  SmallU16String<32> result{};

  // Don't use a StringView iterator, as any calls to createStringView can
  // allocate and move the underlying char storage.
  for (size_t i = 0, e = replacementView.length(); i < e;) {
    // A single $ substitution can expand to the whole input string, so result
    // can grow past MAX_STRING_LENGTH before the bounds check in
    // StringPrimitive::create() below runs. Bail out early with a RangeError
    // instead, as overflowing the intermediate SmallVector would crash.
    //
    // Checking once per iteration is enough to prevent that overflow:
    // everything an iteration can append ($&, $`, $', $n, and $<name> via
    // toString_RJS) is a StringPrimitive and so is at most MAX_STRING_LENGTH
    // long, which bounds result at 2 * MAX_STRING_LENGTH. SmallVector's
    // capacity is an unsigned whose doubling wraps above 2^31 elements, and
    // that bound has to stay clear of it.
    static_assert(
        2ull * StringPrimitive::MAX_STRING_LENGTH < (1ull << 31),
        "result can overflow SmallVector's capacity");
    if (LLVM_UNLIKELY(result.size() > StringPrimitive::MAX_STRING_LENGTH))
      return runtime.raiseRangeError("String length exceeds limit");
    // Go character by character and account for $ replacement strings.
    char16_t c0 = replacementView[i];
    if (c0 != u'$' || i + 1 == e) {
      // Not a special replacement string, just add the character.
      result.append(c0);
      ++i;
      continue;
    }

    // There's a character after the '$', check for replacement strings.
    char16_t c1 = replacementView[i + 1];
    if (c1 == u'$') {
      result.append(u'$');
      i += 2;
    } else if (c1 == u'&') {
      // The matched substring.
      matchedStrView.appendUTF16String(result);
      i += 2;
    } else if (c1 == u'`') {
      // Portion of string before the matched substring.
      stringView.slice(0, position).appendUTF16String(result);
      i += 2;
    } else if (c1 == u'\'') {
      // Portion of string after the matched substring.
      if (tailPos < stringLength) {
        stringView.slice(position + matchLength).appendUTF16String(result);
      }
      i += 2;
    } else if (u'0' <= c1 && c1 <= u'9') {
      // $n index.
      // '0' <= c1 <= '9' because $nn case can have 01 to 99.
      // If it ends up being the $n case instead of $nn,
      // then we can check to make sure 1 <= n <= 9.

      // Define a helper to access a submatch substring, or the empty
      // string if the submatch is undefined.
      auto submatchOrEmpty = [&](size_t idx) -> StringView {
        assert(
            captures && idx < captures->size() &&
            "Index into captures is out of bound.");
        if (captures->at(idx).isUndefined()) {
          // return empty string.
          return stringView.slice(str->getStringLength());
        }
        lv.captureStrHandle = captures->at(idx).getString(runtime);
        return StringPrimitive::createStringView(runtime, lv.captureStrHandle);
      };

      uint32_t n = c1 - u'0';
      if (i + 2 < e) {
        // Try for the $nn case if there's more characters available.
        char16_t c2 = replacementView[i + 2];
        // $nn index.
        uint32_t nn = (c1 - u'0') * 10 + (c2 - u'0');
        if ((u'0' <= c2 && c2 <= u'9') && (1 <= nn && nn <= m)) {
          // Valid $nn case.
          auto view = submatchOrEmpty(nn - 1);
          result.insert(result.end(), view.begin(), view.end());
          i += 3;
        } else if (1 <= n && n <= m) {
          // Try for the $n case first.
          auto view = submatchOrEmpty(n - 1);
          result.insert(result.end(), view.begin(), view.end());
          i += 2;
        } else {
          // No valid $n or $nn case, just append the characters and
          // move on.
          result.append({c0, c1});
          i += 2;
        }
      } else if (1 <= n && n <= m) {
        auto view = submatchOrEmpty(n - 1);
        result.insert(result.end(), view.begin(), view.end());
        i += 2;
      } else {
        // Not a valid $n.
        result.append({c0, c1});
        i += 2;
      }
    } else if (c1 == u'<' && namedCaptures) {
      // i. Let gtPos be StringIndxOf(templateRemainder, ">", 0).
      size_t gtPos = 0;
      for (size_t innerI = i + 2; innerI < e; innerI++) {
        if (replacementView[innerI] == u'>') {
          gtPos = innerI;
          break;
        }
      }
      // We couldn't find a valid identifier
      if (gtPos == 0) {
        result.append({c0, c1});
        i += 2;
      } else {
        llvh::SmallVector<char16_t, 32> storage;
        // 2. Let groupName be the substring of templateRemainder from 2 to
        // gtPos.
        // Copy the name out of the replacement string: getSymbolHandle may
        // allocate when creating a new identifier, which can move the string.
        replacementView.slice(i + 2, gtPos - (i + 2))
            .appendUTF16String(storage);
        auto symbolRes = runtime.getIdentifierTable().getSymbolHandle(
            runtime, UTF16Ref(storage));
        if (LLVM_UNLIKELY(symbolRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        auto captureRes =
            JSObject::getNamed_RJS(namedCaptures, runtime, symbolRes->get());
        if (LLVM_UNLIKELY(captureRes == ExecutionStatus::EXCEPTION))
          return ExecutionStatus::EXCEPTION;
        // 5. If capture is undefined, then
        // a. Let refReplacement be the empty String.
        // 6. Else,
        if (!(*captureRes)->isUndefined()) {
          // a. Let refReplacement be ? ToString(capture).
          lv.captureValueHandle = std::move(*captureRes);
          auto toStrRes = toString_RJS(runtime, lv.captureValueHandle);
          if (toStrRes == ExecutionStatus::EXCEPTION) {
            return ExecutionStatus::EXCEPTION;
          }
          (*toStrRes)->appendUTF16String(result);
        }
        // Advance the cursor past the terminating '>'.
        i = gtPos + 1;
      }
    } else {
      // None of the replacement strings count, add both characters.
      result.append({c0, c1});
      i += 2;
    }
  }
  // 12. Return result.
  return StringPrimitive::create(runtime, result);
}

/// ES11 21.2.5.8
/// TODO(T69340954): make this compliant once we support species constructor.
CallResult<HermesValue> regExpPrototypeSymbolMatchAll(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};

  struct : public Locals {
    PinnedValue<StringPrimitive> S;
    PinnedValue<StringPrimitive> flags;
    PinnedValue<JSRegExp> matcher;
    PinnedValue<> propValueHandle;
    PinnedValue<> flagsPropHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 1. Let R be the this value.
  auto R = args.dyncastThis<JSObject>();

  // 2. If Type(R) is not Object, throw a TypeError exception.
  if (LLVM_UNLIKELY(!R)) {
    return runtime.raiseTypeError(
        "RegExp.prototype[@@matchAll] should be called on a js object");
  }

  // 3. Let S be ? ToString(string).
  auto strRes = toString_RJS(runtime, args.getArgHandle(0));
  if (strRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);

  // 4. Let C be ? SpeciesConstructor(R, %RegExp%).
  // Since species constructors is not supported, C is always %RegExp%.

  // 5. Let flags be ? ToString(? Get(R, "flags")).
  auto flagsPropRes = JSObject::getNamed_RJS(
      R, runtime, Predefined::getSymbolID(Predefined::flags));
  if (LLVM_UNLIKELY(flagsPropRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.flagsPropHandle = std::move(*flagsPropRes);
  auto flagsStrRes = toString_RJS(runtime, lv.flagsPropHandle);
  if (LLVM_UNLIKELY(flagsStrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.flags = std::move(*flagsStrRes);

  // 6. Let matcher be ? Construct(C, « R, flags »).
  // This is thus equivalent to RegExp(« R, flags »).
  // it is necessary to invoke the 21.2.3.1 RegExp, neither RegExpCreate
  // nor internal JSRegExp creation can shortcut without diverging the spec.
  auto newRegExpRes = regExpConstructorFastCopy(
      runtime, R, lv.flags, MutableHandle<JSRegExp>{lv.matcher});
  if (newRegExpRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  // 7. Let lastIndex be ? ToLength(? Get(R, "lastIndex")).
  auto propRes = JSObject::getNamed_RJS(
      R, runtime, Predefined::getSymbolID(Predefined::lastIndex));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.propValueHandle = std::move(*propRes);
  auto lenRes = toLength(runtime, lv.propValueHandle);
  if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double lastIndex = lenRes->getNumber();
  // 8. Perform ? Set(matcher, "lastIndex", lastIndex, true).
  if (setLastIndex(lv.matcher, runtime, lastIndex) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  // 9. If flags contains "g", let global be true.
  // 10. Else, let global be false.
  // 11. If flags contains "u", let fullUnicode be true.
  // 12. Else, let fullUnicode be false.
  bool global = false;
  bool fullUnicode = false;
  auto strView = StringPrimitive::createStringView(runtime, lv.flags);
  for (auto it = strView.begin(); it < strView.end(); it++) {
    if (*it == 'g')
      global = true;
    if (*it == 'u')
      fullUnicode = true;
  }

  // 13. Return ! CreateRegExpStringIterator(matcher, S, global, fullUnicode).
  return JSRegExpStringIterator::create(
             runtime, lv.matcher, lv.S, global, fullUnicode)
      .getHermesValue();
}

// ES6 21.2.5.14
// Note there is no requirement that 'this' be a RegExp object.
CallResult<HermesValue> regExpPrototypeToString(void *, Runtime &runtime) {
  struct : public Locals {
    PinnedValue<> sourceHandle;
    PinnedValue<StringPrimitive> pattern;
    PinnedValue<> flagsHandle;
    PinnedValue<StringPrimitive> flags;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  Handle<JSObject> regexp = args.dyncastThis<JSObject>();
  if (!regexp) {
    return runtime.raiseTypeError(
        "RegExp.prototype.toString() called on non-object");
  }

  // Let pattern be ToString(Get(R, "source"))
  auto source = JSObject::getNamed_RJS(
      regexp, runtime, Predefined::getSymbolID(Predefined::source));
  if (LLVM_UNLIKELY(source == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.sourceHandle = std::move(*source);
  auto patternRes = toString_RJS(runtime, lv.sourceHandle);
  if (LLVM_UNLIKELY(patternRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.pattern = std::move(*patternRes);

  // Let flags be ToString(Get(R, "flags"))
  auto flagsObj = JSObject::getNamed_RJS(
      regexp, runtime, Predefined::getSymbolID(Predefined::flags));
  if (LLVM_UNLIKELY(flagsObj == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.flagsHandle = std::move(*flagsObj);
  auto flagsRes = toString_RJS(runtime, lv.flagsHandle);
  if (LLVM_UNLIKELY(flagsRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.flags = std::move(*flagsRes);

  // 'Let result be the String value formed by concatenating "/", pattern, and
  // "/", and flags.' We expect 2 slashes plus at most 5 flags.
  SmallU16String<32> result;
  result.reserve(lv.pattern->getStringLength() + 2 + 5);

  result.push_back(u'/');
  lv.pattern->appendUTF16String(result);
  result.push_back(u'/');
  lv.flags->appendUTF16String(result);
  return StringPrimitive::create(runtime, result);
}

/// \return true if \p R is a JSRegExp and Get(R, "exec") would return the
/// builtin exec from a plain data property, with no proxy, host object or
/// accessor on the lookup path. Such a Get runs no user code, so skipping
/// it is unobservable.
static bool hasBuiltinExec(Runtime &runtime, Handle<JSObject> R) {
  if (!vmisa<JSRegExp>(R.getHermesValue()))
    return false;
  NamedPropertyDescriptor desc;
  JSObject *owner = JSObject::getNamedDescriptorPredefined(
      R, runtime, Predefined::exec, desc);
  if (!owner || desc.flags.proxyObject || desc.flags.hostObject ||
      desc.flags.accessor)
    return false;
  SmallHermesValue slot =
      JSObject::getNamedSlotValueUnsafe(owner, runtime, desc);
  return slot.isObject() &&
      slot.getObject(runtime) == *runtime.regExpPrototypeExec;
}

/// Run regExpBuiltinExecMatch(\p R, \p S, \p match) inside a frame for the
/// builtin exec, as a call of RegExp.prototype.exec would. \return whether
/// it matched.
static CallResult<bool> execMatchInBuiltinExecFrame(
    Runtime &runtime,
    Handle<JSRegExp> R,
    Handle<StringPrimitive> S,
    RegExpMatch &match) {
  bool matched = false;
  auto res =
      runInBuiltinExecFrame(runtime, R, S, [&]() -> CallResult<HermesValue> {
        auto matchRes = regExpBuiltinExecMatch(R, runtime, S, match);
        if (LLVM_UNLIKELY(matchRes == ExecutionStatus::EXCEPTION))
          return ExecutionStatus::EXCEPTION;
        matched = *matchRes;
        return HermesValue::encodeUndefinedValue();
      });
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return matched;
}

/// RegExpExec(\p rx, \p S) for @@match, @@search or @@replace. If \p fast
/// (hasBuiltinExec(rx) held), run the builtin exec in its frame and store
/// the match in \p match, leaving \p result untouched; otherwise run
/// RegExpExec and store its result in \p result, and \p match is
/// meaningless. \return whether there was a match (the result is not null).
static CallResult<bool> execBuiltinOrGeneric(
    Runtime &runtime,
    bool fast,
    Handle<JSObject> rx,
    Handle<StringPrimitive> S,
    RegExpMatch &match,
    MutableHandle<> result) {
  if (fast)
    return execMatchInBuiltinExecFrame(
        runtime, Handle<JSRegExp>::vmcast(rx), S, match);
  auto execRes = regExpExec(runtime, rx, S);
  if (LLVM_UNLIKELY(execRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  result.set(*execRes);
  return !result->isNull();
}

/// After an empty match in the loop of @@match or @@replace: set lastIndex
/// of \p rx to AdvanceStringIndex(\p S, ToLength(Get(rx, "lastIndex")),
/// \p fullUnicode).
static ExecutionStatus advanceLastIndexAfterEmptyMatch(
    Runtime &runtime,
    Handle<JSObject> rx,
    Handle<StringPrimitive> S,
    bool fullUnicode) {
  struct : public Locals {
    PinnedValue<> lastIndex;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // a. Let thisIndex be ToLength(Get(rx, "lastIndex")).
  // b. ReturnIfAbrupt(thisIndex).
  auto propRes = runtime.getNamed(rx, PropCacheID::RegExpLastIndex);
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  lv.lastIndex = std::move(*propRes);
  auto thisIndex = toLength(runtime, lv.lastIndex);
  if (LLVM_UNLIKELY(thisIndex == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  // c. Let nextIndex be AdvanceStringIndex(S, thisIndex, fullUnicode).
  double nextIndex = advanceStringIndex(
      S.get(), thisIndex->getNumberAs<uint64_t>(), fullUnicode);
  // d. Let setStatus be Set(rx, "lastIndex", nextIndex, true).
  // e. ReturnIfAbrupt(setStatus).
  return setLastIndex(rx, runtime, nextIndex);
}

// TODO: consider writing this in JS.
/// ES6.0 21.2.5.6
CallResult<HermesValue> regExpPrototypeSymbolMatch(void *, Runtime &runtime) {
  struct : public Locals {
    PinnedValue<> propValue;
    PinnedValue<> result;
    PinnedValue<StringPrimitive> matchStr;
    PinnedValue<StringPrimitive> S;
    PinnedValue<JSArray> A;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};

  // 1. Let rx be the this value.
  Handle<JSObject> rx = args.dyncastThis<JSObject>();

  // 2. If Type(rx) is not Object, throw a TypeError exception.
  if (LLVM_UNLIKELY(!rx)) {
    return runtime.raiseTypeError(
        "RegExp.prototype[@@match] should be called on a js object");
  }
  // 3. Let S be ToString(string)
  // 4. ReturnIfAbrupt(S).
  auto strRes = toString_RJS(runtime, args.getArgHandle(0));
  if (strRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);
  // 5. Let global be ToBoolean(Get(rx, "global")).
  // 6. ReturnIfAbrupt(global).
  auto propRes = JSObject::getNamed_RJS(
      rx, runtime, Predefined::getSymbolID(Predefined::global));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto global = toBoolean(propRes->get());
  // 7. If global is false, then
  //   a. Return RegExpExec(rx, S).
  if (!global) {
    return regExpExec(runtime, rx, lv.S);
  }
  // 8. Else global is true,
  // a. Let fullUnicode be ToBoolean(Get(rx, "unicode"))
  // b. ReturnIfAbrupt(fullUnicode)
  auto unicodePropRes = JSObject::getNamed_RJS(
      rx, runtime, Predefined::getSymbolID(Predefined::unicode));
  if (LLVM_UNLIKELY(unicodePropRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  bool fullUnicode = toBoolean(unicodePropRes->get());

  // c. Let setStatus be Set(rx, "lastIndex", 0, true).
  // d. ReturnIfAbrupt(setStatus).
  if (setLastIndex(rx, runtime, 0) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // e. Let A be ArrayCreate(0).
  auto arrRes = JSArray::create(runtime, 0, 0);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.A = std::move(*arrRes);
  // e. Let n be 0.
  uint32_t n = 0;

  // Whether exec is the builtin. Besides exec, every step of the loop below
  // runs no user code (ToString of a string, Get and ToLength of the number
  // that lastIndex holds, Set of lastIndex), so exec cannot change during
  // the loop and one check covers every iteration.
  const bool fast = hasBuiltinExec(runtime, rx);

  // g. Repeat,
  auto marker = gcScope.createMarker();
  RegExpMatch match;
  while (true) {
    gcScope.flushToMarker(marker);
    // i. Let result be RegExpExec(rx, S).
    // ii. ReturnIfAbrupt(result).
    auto matchedRes =
        execBuiltinOrGeneric(runtime, fast, rx, lv.S, match, lv.result);
    if (LLVM_UNLIKELY(matchedRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // iii. If result is null, then
    if (!*matchedRes) {
      if (n == 0) {
        return HermesValue::encodeNullValue();
      } else {
        if (LLVM_UNLIKELY(
                JSArray::setLengthProperty(lv.A, runtime, n) ==
                ExecutionStatus::EXCEPTION))
          return ExecutionStatus::EXCEPTION;
        return lv.A.getHermesValue();
      }
    }
    // iv. Else result is not null,
    // 1. Let matchStr be ToString(Get(result, "0")).
    // 2. ReturnIfAbrupt(matchStr).
    if (fast) {
      // Element 0 of the builtin's result is this slice of S.
      auto strRes = StringPrimitive::slice(
          runtime, lv.S, match[0]->location, match[0]->length);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.matchStr.castAndSetHermesValue<StringPrimitive>(*strRes);
    } else {
      auto resultObj = Handle<JSObject>::vmcast(&lv.result);
      auto propRes2 = getIndexed_RJS(runtime, resultObj, 0);
      if (propRes2 == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.propValue = std::move(propRes2.getValue());
      auto strRes2 = toString_RJS(runtime, lv.propValue);
      if (strRes2 == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.matchStr = strRes2->get();
    }
    // 3. Let status be CreateDataProperty(A, ToString(n), matchStr).
    // 4. Assert: status is true.
    if (LLVM_UNLIKELY(
            JSArray::setElementAt(lv.A, runtime, n, lv.matchStr) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    // 5. If matchStr is the empty String, then
    //   a-e. Set lastIndex to AdvanceStringIndex(S, thisIndex, fullUnicode).
    if (lv.matchStr->getStringLength() == 0) {
      if (LLVM_UNLIKELY(
              advanceLastIndexAfterEmptyMatch(runtime, rx, lv.S, fullUnicode) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    }
    // 6. Increment n.
    n++;
  }
  llvm_unreachable(
      "RegExp.prototype[@@match] must stop when all matched results are returned.");
}

/// ES6.0 21.2.5.9
CallResult<HermesValue> regExpPrototypeSymbolSearch(void *, Runtime &runtime) {
  struct : public Locals {
    PinnedValue<StringPrimitive> S;
    PinnedValue<> previousLastIndex;
    PinnedValue<> result;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};

  // 1. Let rx be the this value.
  // 2. If Type(rx) is not Object, throw a TypeError exception.
  Handle<JSObject> rx = args.dyncastThis<JSObject>();
  if (!rx) {
    return runtime.raiseTypeError(
        "Calling regExp.prototype[@@search] on a non-object.");
  }
  // 3. Let S be ToString(string).
  // 4. ReturnIfAbrupt(S).
  auto strRes = toString_RJS(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);
  // 5. Let previousLastIndex be Get(rx, "lastIndex").
  // 6. ReturnIfAbrupt(previousLastIndex).
  auto propRes = runtime.getNamed(rx, PropCacheID::RegExpLastIndex);
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.previousLastIndex = std::move(propRes.getValue());
  // 7. Let status be Set(rx, "lastIndex", 0, true).
  auto status = setLastIndex(rx, runtime, 0);
  // 8. ReturnIfAbrupt(status).
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 9. Let result be RegExpExec(rx, S).
  // 10. ReturnIfAbrupt(result).
  // When exec is the builtin, only the position of the match is needed.
  const bool fast = hasBuiltinExec(runtime, rx);
  RegExpMatch match;
  auto matchedRes =
      execBuiltinOrGeneric(runtime, fast, rx, lv.S, match, lv.result);
  if (LLVM_UNLIKELY(matchedRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  const bool matched = *matchedRes;
  // 11. Let status be Set(rx, "lastIndex", previousLastIndex, true).
  auto previousLastIndexSHV = SmallHermesValue::encodeHermesValue(
      lv.previousLastIndex.getHermesValue(), runtime);
  status = setLastIndex(rx, runtime, previousLastIndexSHV);
  // 12. ReturnIfAbrupt(status).
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 13. If result is null, return -1.
  if (!matched) {
    return HermesValue::encodeTrustedNumberValue(-1);
  }
  // 14. Return Get(result, "index").
  if (fast) {
    // The builtin's index is the start of the whole match.
    return HermesValue::encodeTrustedNumberValue(match[0]->location);
  }
  auto resultObj = Handle<JSObject>::dyn_vmcast(Handle<>{lv.result});
  if (LLVM_UNLIKELY(!resultObj)) {
    return ExecutionStatus::EXCEPTION;
  }
  return JSObject::getNamed_RJS(
             resultObj, runtime, Predefined::getSymbolID(Predefined::index))
      .toCallResultHermesValue();
}

/// ES6.0 21.2.5.8 steps 16.m-p of RegExp.prototype[@@replace], for one
/// match: compute the replacement of the match \p matched at \p position of
/// \p S, whose captures are \p captures (each a string or undefined) and
/// whose named captures are \p namedCaptures (undefined if none), by calling
/// \p replaceFn if it is not null and by GetSubstitution with
/// \p replaceValueStr otherwise. If \p position is not before
/// \p nextSourcePosition, append the text of \p stringView (a view of S)
/// from nextSourcePosition up to position, and the replacement, to
/// \p accumulatedResult, and move \p nextSourcePosition past the match.
static ExecutionStatus replaceOneMatch(
    Runtime &runtime,
    Handle<StringPrimitive> S,
    const StringView &stringView,
    Handle<Callable> replaceFn,
    Handle<StringPrimitive> replaceValueStr,
    Handle<StringPrimitive> matched,
    uint32_t position,
    Handle<ArrayStorageSmall> captures,
    Handle<> namedCaptures,
    SmallU16String<32> &accumulatedResult,
    uint32_t &nextSourcePosition) {
  struct : public Locals {
    PinnedValue<HermesValue> namedCapturesObj;
    PinnedValue<> replValue;
    PinnedValue<StringPrimitive> replacement;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  const bool hasNamedCaptures = !namedCaptures->isUndefined();
  // f. Let matchLength be the number of code units in matched.
  const uint32_t matchLength = matched->getStringLength();

  // m. If functionalReplace is true, then
  if (replaceFn) {
    CallResult<PseudoHandle<>> callRes{ExecutionStatus::EXCEPTION};
    {
      // i. Let replacerArgs be «matched».
      // Arguments: matched, captures, position, S (, groups).
      size_t replacerArgsCount =
          1 + (size_t)captures->size() + 2 + hasNamedCaptures;
      if (LLVM_UNLIKELY(replacerArgsCount >= UINT32_MAX))
        return runtime.raiseStackOverflow(
            Runtime::StackOverflowKind::JSRegisterStack);
      ScopedNativeCallFrame newFrame{
          runtime,
          static_cast<uint32_t>(replacerArgsCount),
          *replaceFn,
          false,
          HermesValue::encodeUndefinedValue()};
      if (LLVM_UNLIKELY(newFrame.overflowed()))
        return runtime.raiseStackOverflow(
            Runtime::StackOverflowKind::NativeStack);

      uint32_t argIdx = 0;
      newFrame->getArgRef(argIdx++) = matched.getHermesValue();
      // ii. Append in list order the elements of captures to the end of the
      // List replacerArgs.
      for (; argIdx <= captures->size(); ++argIdx) {
        newFrame->getArgRef(argIdx) =
            captures->at(argIdx - 1).unboxToHV(runtime);
      }
      // iii. Append position and S.
      newFrame->getArgRef(argIdx++) =
          HermesValue::encodeTrustedNumberValue(position);
      newFrame->getArgRef(argIdx++) = S.getHermesValue();
      // iv. If namedCaptures is not undefined, then
      if (hasNamedCaptures) {
        newFrame->getArgRef(argIdx++) = namedCaptures.getHermesValue();
      }
      // v. Let replValue be Call(replaceValue, undefined, replacerArgs).
      callRes = Callable::call(replaceFn, runtime);
      if (callRes == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
    }
    // vi. Let replacement be ToString(replValue).
    lv.replValue = std::move(callRes.getValue());
    auto strRes = toString_RJS(runtime, lv.replValue);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.replacement = strRes->get();
  } else {
    // n. Else,
    // i. If namedCaptures is not undefined, then
    if (hasNamedCaptures) {
      // 1. Set namedCaptures to ? ToObject(namedCaptures).
      auto objRes = toObject(runtime, namedCaptures);
      if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.namedCapturesObj = *objRes;
    }
    // ii. Let replacement be GetSubstitution(matched, S, position, captures,
    // replaceValue).
    auto callRes = getSubstitution(
        runtime,
        matched,
        S,
        position,
        captures,
        hasNamedCaptures ? Handle<JSObject>::vmcast(&lv.namedCapturesObj)
                         : Runtime::makeNullHandle<JSObject>(),
        replaceValueStr);
    if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.replacement.castAndSetHermesValue<StringPrimitive>(callRes.getValue());
  }
  // o. ReturnIfAbrupt(replacement).
  // p. If position ≥ nextSourcePosition, then
  if (position >= nextSourcePosition) {
    // i. NOTE position should not normally move backwards. If it does, it is
    // an indication of an ill-behaving RegExp subclass or use of an access
    // triggered side-effect to change the global flag or other
    // characteristics of rx. In such cases, the corresponding substitution is
    // ignored.
    // ii. Let accumulatedResult be the String formed by concatenating the
    // code units of the current value of accumulatedResult with the
    // substring of S consisting of the code units from nextSourcePosition
    // (inclusive) up to position (exclusive) and with the code units of
    // replacement.
    stringView.slice(nextSourcePosition, position - nextSourcePosition)
        .appendUTF16String(accumulatedResult);
    lv.replacement->appendUTF16String(accumulatedResult);
    // iii. Let nextSourcePosition be position + matchLength.
    nextSourcePosition = position + matchLength;
  }
  return ExecutionStatus::RETURNED;
}

/// Phase 2 of RegExp.prototype[@@replace] for a match \p match of the
/// builtin exec on \p S, given as its ranges in RegExpMatch order: build
/// matched, the captures and the groups object (from \p groupNames, the
/// regexp's group-name mapping, if not null) as the builtin's result array
/// would hold them, then run replaceOneMatch().
/// The Gets this replaces read own properties of an array user code never
/// sees, so skipping them is unobservable.
static ExecutionStatus replaceBuiltinMatch(
    Runtime &runtime,
    Handle<StringPrimitive> S,
    const StringView &stringView,
    Handle<Callable> replaceFn,
    Handle<StringPrimitive> replaceValueStr,
    Handle<JSObject> groupNames,
    llvh::ArrayRef<OptValue<RegExpMatchRange>> match,
    SmallU16String<32> &accumulatedResult,
    uint32_t &nextSourcePosition) {
  struct : public Locals {
    PinnedValue<StringPrimitive> matched;
    PinnedValue<ArrayStorageSmall> captures;
    PinnedValue<> capN;
    PinnedValue<> namedCaptures;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // matched: element 0, the whole match.
  const RegExpMatchRange whole = *match[0];
  auto matchedRes =
      StringPrimitive::slice(runtime, S, whole.location, whole.length);
  if (LLVM_UNLIKELY(matchedRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  lv.matched.castAndSetHermesValue<StringPrimitive>(*matchedRes);

  // captures: a string per matched group, undefined per unmatched one.
  const uint32_t nCaptures = match.size() - 1;
  auto capturesRes = ArrayStorageSmall::create(runtime, nCaptures);
  if (LLVM_UNLIKELY(capturesRes == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  lv.captures.castAndSetHermesValue<ArrayStorageSmall>(*capturesRes);
  MutableHandle<ArrayStorageSmall> captures{lv.captures};
  for (uint32_t n = 1; n <= nCaptures; ++n) {
    GCScopeMarkerRAII marker{runtime};
    if (!match[n]) {
      lv.capN = HermesValue::encodeUndefinedValue();
    } else {
      auto capRes = StringPrimitive::slice(
          runtime, S, match[n]->location, match[n]->length);
      if (LLVM_UNLIKELY(capRes == ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      lv.capN = *capRes;
    }
    if (LLVM_UNLIKELY(
            ArrayStorageSmall::push_back(captures, runtime, lv.capN) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
  }

  // namedCaptures: the groups object, or undefined without named groups.
  if (groupNames) {
    auto groupsRes = buildGroupsObject(
        runtime, groupNames, [&](uint32_t i) -> SmallHermesValue {
          return captures->at(i - 1);
        });
    if (LLVM_UNLIKELY(groupsRes == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    lv.namedCaptures = groupsRes->getHermesValue();
  } else {
    lv.namedCaptures = HermesValue::encodeUndefinedValue();
  }

  return replaceOneMatch(
      runtime,
      S,
      stringView,
      replaceFn,
      replaceValueStr,
      lv.matched,
      whole.location,
      captures,
      lv.namedCaptures,
      accumulatedResult,
      nextSourcePosition);
}

/// ES6.0 21.2.5.8
CallResult<HermesValue> regExpPrototypeSymbolReplace(void *, Runtime &runtime) {
  struct : public Locals {
    PinnedValue<StringPrimitive> replaceValueStr;
    PinnedValue<ArrayStorageSmall> resultsHandle;
    PinnedValue<> propValue;
    PinnedValue<StringPrimitive> matchStr;
    PinnedValue<JSObject> result;
    PinnedValue<StringPrimitive> S;
    PinnedValue<> valueHandle;
    PinnedValue<> indexedPropValue;
    PinnedValue<StringPrimitive> matched;
    PinnedValue<> indexPropValue;
    PinnedValue<ArrayStorageSmall> capturesHandle;
    PinnedValue<> capN;
    PinnedValue<HermesValue> namedCaptures;
    PinnedValue<> execResult;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};

  // 1. Let rx be the this value.
  // 2. If Type(rx) is not Object, throw a TypeError exception.
  Handle<JSObject> rx = args.dyncastThis<JSObject>();
  if (LLVM_UNLIKELY(!rx)) {
    return runtime.raiseTypeError(
        "RegExp.prototype[@@replace] called on a non-object.");
  }
  // 3. Let S be ToString(string).
  // 4. ReturnIfAbrupt(S).
  auto strRes = toString_RJS(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);
  // 5. Let lengthS be the number of code unit elements in S.
  uint32_t lengthS = lv.S->getStringLength();
  // 6. Let functionalReplace be IsCallable(replaceValue).
  Handle<> replaceValue = args.getArgHandle(1);
  auto replaceFn = Handle<Callable>::dyn_vmcast(replaceValue);
  // 7. If functionalReplace is false, then
  if (!replaceFn) {
    //   a. Let replaceValue be ToString(replaceValue).
    //   b. ReturnIfAbrupt(replaceValue).
    auto strRes = toString_RJS(runtime, replaceValue);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.replaceValueStr = strRes->get();
  }
  // 8. Let global be ToBoolean(Get(rx, "global")).
  // 9. ReturnIfAbrupt(global).
  auto propRes = JSObject::getNamed_RJS(
      rx, runtime, Predefined::getSymbolID(Predefined::global));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  bool global = toBoolean(propRes->get());

  // Note: fullUnicode is only used if global is set.
  bool fullUnicode = false;

  // 10. If global is true, then
  if (global) {
    //   a. Let fullUnicode be ToBoolean(Get(rx, "unicode")).
    //   b. ReturnIfAbrupt(fullUnicode).
    auto unicodePropRes = JSObject::getNamed_RJS(
        rx, runtime, Predefined::getSymbolID(Predefined::unicode));
    if (LLVM_UNLIKELY(unicodePropRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    fullUnicode = toBoolean(unicodePropRes->get());

    //   c. Let setStatus be Set(rx, "lastIndex", 0, true).
    auto setStatus = setLastIndex(rx, runtime, 0);
    //   d. ReturnIfAbrupt(setStatus).
    if (LLVM_UNLIKELY(setStatus == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  // Whether exec is the builtin. From here to the end of phase 1 no user
  // code runs outside exec (ToString of a string, Get and ToLength of the
  // number that lastIndex holds, Set of lastIndex), so one check covers
  // every exec; callbacks run in phase 2, after the last exec.
  const bool fast = hasBuiltinExec(runtime, rx);
  // The matches of phase 1 when fast, back to back: each holds
  // fastMatchStride ranges, the whole match and one per group. One flat
  // vector keeps the memory outside the GC heap to the ranges themselves.
  std::vector<OptValue<RegExpMatchRange>> fastMatches;
  size_t fastMatchStride = 0;
  MutableHandle<ArrayStorageSmall> resultsHandle{lv.resultsHandle};
  if (!fast) {
    // 11. Let results be a new empty List.
    auto arrRes = ArrayStorageSmall::create(runtime, 16 /* capacity */);
    if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.resultsHandle.castAndSetHermesValue<ArrayStorageSmall>(
        arrRes.getValue());
  }

  // 12. Let done be false.
  // 13. Repeat, while done is false
  auto stringView = StringPrimitive::createStringView(runtime, lv.S);
  while (true) {
    GCScopeMarkerRAII marker{runtime};
    bool emptyMatch = false;
    // a. Let result be RegExpExec(rx, S).
    // b. ReturnIfAbrupt(result).
    RegExpMatch match;
    auto matchedRes =
        execBuiltinOrGeneric(runtime, fast, rx, lv.S, match, lv.execResult);
    if (LLVM_UNLIKELY(matchedRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // c. If result is null, set done to true.
    if (!*matchedRes)
      break;
    if (fast) {
      // Element 0 of the builtin's result is this range of S.
      emptyMatch = match[0]->length == 0;
      // d.i. Append result to the end of results.
      assert(
          (fastMatchStride == 0 || fastMatchStride == match.size()) &&
          "every match of a regexp has the same number of ranges");
      fastMatchStride = match.size();
      fastMatches.insert(fastMatches.end(), match.begin(), match.end());
      // ii. If global is false, set done to true.
      if (!global)
        break;
    } else {
      // d. Else result is not null,
      // If result is not null, it must be an object.
      lv.result.castAndSetHermesValue<JSObject>(lv.execResult.get());
      // i. Append result to the end of results.
      if (LLVM_UNLIKELY(
              ArrayStorageSmall::push_back(resultsHandle, runtime, lv.result) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      // ii. If global is false, set done to true.
      if (!global)
        break;
      // iii. Else,
      // 1. Let matchStr be ToString(Get(result, "0")).
      // 2. ReturnIfAbrupt(matchStr).
      propRes = getIndexed_RJS(runtime, lv.result, 0);
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.propValue = propRes->get();
      auto strRes = toString_RJS(runtime, lv.propValue);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.matchStr = strRes->get();
      emptyMatch = lv.matchStr->getStringLength() == 0;
    }
    // iii.3. If matchStr is the empty String, then
    //   a-e. Set lastIndex to AdvanceStringIndex(S, thisIndex, fullUnicode).
    if (emptyMatch) {
      if (LLVM_UNLIKELY(
              advanceLastIndexAfterEmptyMatch(runtime, rx, lv.S, fullUnicode) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    }
  }

  // 14. Let accumulatedResult be the empty String value.
  SmallU16String<32> accumulatedResult{};
  // 15. Let nextSourcePosition be 0.
  uint32_t nextSourcePosition = 0;
  // 16. Repeat, for each result in results,
  if (fast) {
    Handle<JSObject> groupNames =
        Handle<JSRegExp>::vmcast(rx)->getGroupNameMappings(runtime);
    for (size_t i = 0, e = fastMatches.size(); i < e; i += fastMatchStride) {
      GCScopeMarkerRAII marker1{runtime};
      if (LLVM_UNLIKELY(
              replaceBuiltinMatch(
                  runtime,
                  lv.S,
                  stringView,
                  replaceFn,
                  lv.replaceValueStr,
                  groupNames,
                  llvh::makeArrayRef(&fastMatches[i], fastMatchStride),
                  accumulatedResult,
                  nextSourcePosition) == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    }
  } else {
    for (uint32_t i = 0, size = resultsHandle->size(); i < size; ++i) {
      GCScopeMarkerRAII marker1{runtime};
      lv.result = vmcast<JSObject>(resultsHandle->at(i).getObject(runtime));
      // a. Let nCaptures be ToLength(Get(result, "length")).
      // b. ReturnIfAbrupt(nCaptures).
      propRes = JSObject::getNamed_RJS(
          lv.result, runtime, Predefined::getSymbolID(Predefined::length));
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.valueHandle = std::move(propRes.getValue());
      auto lengthRes = toLength(runtime, lv.valueHandle);
      if (LLVM_UNLIKELY(lengthRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      // Use uint64_t so it can fit the result of toLength.
      uint64_t nCaptures = lengthRes->getNumberAs<uint64_t>();
      // c. Let nCaptures be max(nCaptures − 1, 0).
      nCaptures = nCaptures > 0 ? nCaptures - 1 : 0;

      // Throw RangeError if nCaptures exceeds uint32_t, since
      // ArrayStorageSmall::create() takes uint32_t capacity and the loop
      // below would run for an impractical number of iterations.
      if (LLVM_UNLIKELY(nCaptures > UINT32_MAX)) {
        return runtime.raiseRangeError(
            "Too many capture groups in exec result");
      }
      // d. Let matched be ToString(Get(result, "0")).
      // e. ReturnIfAbrupt(matched).
      propRes = getIndexed_RJS(runtime, lv.result, 0);
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.indexedPropValue = std::move(propRes.getValue());
      auto strRes = toString_RJS(runtime, lv.indexedPropValue);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.matched = std::move(*strRes);
      // g. Let position be ToIntegerOrInfinity(Get(result, "index")).
      // h. ReturnIfAbrupt(position).
      propRes = JSObject::getNamed_RJS(
          lv.result, runtime, Predefined::getSymbolID(Predefined::index));
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.indexPropValue = std::move(propRes.getValue());
      auto intRes = toIntegerOrInfinity(runtime, lv.indexPropValue);
      if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      // position could potentially be negative or Infinity here, so use
      // double.
      double positionDouble = intRes->getNumber();
      // i. Let position be max(min(position, lengthS), 0).
      // Now we can clamp to uint32_t because we've bounds checked
      // and `lengthS` is `uint32_t`.
      uint32_t position = (int64_t)std::fmax(
          std::fmin(positionDouble, (double)lengthS), (double)0);
      // j. Let n be 1.
      // Match the type of nCaptures.
      uint64_t n = 1;
      // k. Let captures be an empty List.
      auto capturesRes =
          ArrayStorageSmall::create(runtime, static_cast<uint32_t>(nCaptures));
      if (LLVM_UNLIKELY(capturesRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.capturesHandle.castAndSetHermesValue<ArrayStorageSmall>(
          capturesRes.getValue());
      MutableHandle<ArrayStorageSmall> capturesHandle{lv.capturesHandle};
      // l. Repeat while n ≤ nCaptures
      while (n <= nCaptures) {
        GCScopeMarkerRAII marker2{runtime};
        // i. Let capN be Get(result, ToString(n)).
        // ii. ReturnIfAbrupt(capN).
        propRes = getIndexed_RJS(runtime, lv.result, n);
        if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        lv.capN = std::move(propRes.getValue());
        // iii. If capN is not undefined, then
        if (!lv.capN->isUndefined()) {
          // 1. Let capN be ToString(capN).
          // 2. ReturnIfAbrupt(capN).
          auto capStrRes = toString_RJS(runtime, lv.capN);
          if (LLVM_UNLIKELY(capStrRes == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          lv.capN = capStrRes->getHermesValue();
        }
        // iv. Append capN as the last element of captures.
        if (LLVM_UNLIKELY(
                ArrayStorageSmall::push_back(
                    capturesHandle, runtime, lv.capN) ==
                ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        // v. Let n be n+1
        n++;
      }

      // j. Let namedCaptures be ? Get(result, "groups").
      auto namedCapturesRes = JSObject::getNamed_RJS(
          lv.result, runtime, Predefined::getSymbolID(Predefined::groups));
      if (LLVM_UNLIKELY(namedCapturesRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.namedCaptures = std::move(*namedCapturesRes);

      // m-p.
      if (LLVM_UNLIKELY(
              replaceOneMatch(
                  runtime,
                  lv.S,
                  stringView,
                  replaceFn,
                  lv.replaceValueStr,
                  lv.matched,
                  position,
                  capturesHandle,
                  lv.namedCaptures,
                  accumulatedResult,
                  nextSourcePosition) == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    }
  }

  // 17. If nextSourcePosition ≥ lengthS, return accumulatedResult.
  if (nextSourcePosition >= lengthS) {
    return StringPrimitive::createEfficient(runtime, accumulatedResult);
  }
  // 18. Return the String formed by concatenating the code units of
  // accumulatedResult with the substring of S consisting of the code units from
  // nextSourcePosition (inclusive) up through the final code unit of S
  // (inclusive).
  stringView.slice(nextSourcePosition).appendUTF16String(accumulatedResult);
  return StringPrimitive::createEfficient(runtime, accumulatedResult);
}

/// ES11.0 21.2.5.13
/// Note: this implementation does not fully observe ES6 spec behaviors because
/// of lack of support for species constructors.
// TODO(T35212035): make this ES6 compliant once we support species constructor.
CallResult<HermesValue> regExpPrototypeSymbolSplit(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  GCScope gcScope{runtime};

  struct : public Locals {
    PinnedValue<StringPrimitive> S;
    PinnedValue<StringPrimitive> flags;
    PinnedValue<JSRegExp> splitter;
    PinnedValue<JSArray> A;
    PinnedValue<> flagsHandle;
    PinnedValue<> tmpHandle;
    PinnedValue<StringPrimitive> captureStr;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 2. If Type(rx) is not Object, throw a TypeError exception.
  if (LLVM_UNLIKELY(!vmisa<JSObject>(args.getThisArg()))) {
    return runtime.raiseTypeError(
        "Cannot call RegExp.protoype[Symbol.split] on a non-object.");
  }
  // 3. Let S be ToString(string).
  auto strRes = toString_RJS(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.S = std::move(*strRes);

  // 5. Let flags be ? ToString(? Get(rx, "flags")).
  auto regexp = Handle<JSObject>::vmcast(args.getThisHandle());
  CallResult<PseudoHandle<>> flagsRes = JSObject::getNamed_RJS(
      regexp, runtime, Predefined::getSymbolID(Predefined::flags));
  if (LLVM_UNLIKELY(flagsRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.flagsHandle = std::move(flagsRes.getValue());
  CallResult<PseudoHandle<StringPrimitive>> flagsStrRes =
      toString_RJS(runtime, lv.flagsHandle);
  if (LLVM_UNLIKELY(flagsStrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.flags = std::move(flagsStrRes.getValue());

  // 8. If flags contains "y", let newFlags be flags.
  // 9. Else, let newFlags be the string that is the concatenation of flags and
  // "y".
  // NOTE: We do not follow this part of the spec, instead, we just use flags as
  // newFlags and actually strip the sticky flag. See below.
  // 10. Let splitter be Construct(C, «rx, newFlags»).
  auto splitterRes = regExpConstructorFastCopy(
      runtime, regexp, lv.flags, MutableHandle<JSRegExp>{lv.splitter});
  if (LLVM_UNLIKELY(splitterRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // The spec actually tells us to always set the sticky flag to true, but
  // since it is much faster to perform a global search, we set sticky to
  // false and then check the returned index.
  auto stickyFlags = JSRegExp::getSyntaxFlags(lv.splitter.get());
  auto newFlags = stickyFlags;
  newFlags.sticky = 0;
  JSRegExp::setSyntaxFlags(lv.splitter.get(), newFlags);

  // 6. If flags contains "u", let unicodeMatching be true.
  // 7. Else, let unicodeMatching be false.
  bool unicodeMatching = newFlags.unicode;

  // 11. Let A be ArrayCreate(0).
  auto arrRes = JSArray::create(runtime, 0, 0);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.A = std::move(*arrRes);
  // 12. Let lengthA be 0.
  uint32_t lengthA = 0;

  // Limit on the number of items allowed in the result array.
  auto limit = args.getArgHandle(1);
  // 13. If limit is undefined, let lim be 232 - 1; else let lim be ?
  // ToUint32(limit).
  uint32_t lim;
  if (limit->isUndefined()) {
    lim = 0xffffffff; // 2 ^ 32 - 1
  } else {
    auto intRes = toUInt32_RJS(runtime, limit);
    if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    lim = intRes->getNumber();
  }

  // 14. Let size be the length of S.
  uint32_t size = lv.S->getStringLength();

  // 15. Let p be 0.
  // Initialise variable indiciating the end of the last match.
  uint32_t p = 0;

  // 16. If lim = 0, return A.
  if (lim == 0) {
    // Don't want any elements, so we're done.
    return lv.A.getHermesValue();
  }

  // 27. If size = 0, then
  if (size == 0) {
    // a. Let z be RegExpExec(splitter, S).
    auto matchResult = JSRegExp::search(lv.splitter, runtime, lv.S, 0);
    if (LLVM_UNLIKELY(matchResult == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // b. If z is not null, return A.
    else if (!matchResult->empty()) {
      // Matched the entirety of S, so return the empty array.
      return lv.A.getHermesValue();
    }
    // c. Perform CreateDataProperty(A, "0", S).
    // Didn't match S, so add it to the array and return.
    if (LLVM_UNLIKELY(
            JSArray::setElementAt(lv.A, runtime, 0, lv.S) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;
    if (LLVM_UNLIKELY(
            JSArray::setLengthProperty(lv.A, runtime, 1) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;

    // d. Return A.
    return lv.A.getHermesValue();
  }

  // 18. Let q be p.
  // Place to attempt the start of the next match.
  uint32_t q = p;

  auto marker = gcScope.createMarker();

  // 19. Repeat, while q < size
  // Main loop: continue while we have space to find another match.
  while (q < size) {
    gcScope.flushToMarker(marker);

    // a. Perform ? Set(splitter, "lastIndex", q, true).
    // b. Let z be ? RegExpExec(splitter, S).
    // c. If z is null, set q to AdvanceStringIndex(S, q, unicodeMatching).

    // Find the next valid match. We know that q < size.
    // the spec only finds matches at q, but we find matches at or
    // after q, so if it fails, we know we're done.
    auto matchResult = JSRegExp::search(lv.splitter, runtime, lv.S, q);
    if (LLVM_UNLIKELY(matchResult == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;

    RegExpMatch match = std::move(*matchResult);

    if (match.empty() || match[0]->location >= size) {
      // There's no matches between index q and the end of the string, so we're
      // done searching. Note: This behavior differs from the spec
      // implementation, because we check for matches at or after q. However, in
      // line with the spec, we only count matches that start before the end of
      // the string.
      break;
    }

    // d. Else,
    // Found a match, so go ahead and update q and e,
    // such that the match is the range [q,e). Note that we have to specifically
    // update q so it appears as though we've have advanced it iteratively as in
    // the spec.
    q = match[0]->location;
    // i. Let e be ? ToLength(? Get(splitter, "lastIndex")).
    // ii. Set e to min(e, size).
    // Note that JSRegExp::search does not actually modify lastIndex so we just
    // compute the value from the match location.
    uint32_t e = q + match[0]->length;

    // iii. If e = p, set q to AdvanceStringIndex(S, q, unicodeMatching).
    if (e == p) {
      // The end of this match is the same as the end of the last match,
      // so we matched with the empty string.
      // We don't want to match the empty string at this location again,
      // so increment q in order to start the next search at the next position.
      q = advanceStringIndex(lv.S.get(), q, unicodeMatching);
    }
    // iv. Else,
    else {
      // 1. Let T be the String value equal to the substring of S consisting of
      // the code units at indices p (inclusive) through q (exclusive).

      // Found a non-empty string match. Add everything from the last match to
      // the current one to A. This has length q-p because q is the start of the
      // current match, and p was the end (exclusive) of the last match.
      auto strRes = StringPrimitive::slice(runtime, lv.S, p, q - p);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.tmpHandle = *strRes;
      // 2. Perform ! CreateDataPropertyOrThrow(A, ! ToString(lengthA), T).
      if (LLVM_UNLIKELY(
              JSArray::setElementAt(lv.A, runtime, lengthA, lv.tmpHandle) ==
              ExecutionStatus::EXCEPTION))
        return ExecutionStatus::EXCEPTION;
      // 3. Set lengthA to lengthA + 1.
      ++lengthA;

      // 4. If lengthA = lim, return A.
      if (lengthA == lim) {
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

      // 6. Let numberOfCaptures be ? LengthOfArrayLike(z).
      // 7. Set numberOfCaptures to max(numberOfCaptures - 1, 0).
      // 8. Let i be 1.
      // 9. Repeat, while i <= numberOfCaptures.
      // Add all the capture groups to A. Start at i=1 to skip the full match.
      for (uint32_t i = 1, m = match.size(); i < m; ++i) {
        GCScopeMarkerRAII captureMarker{runtime};
        // a. Let nextCapture be ? Get(z, ! ToString(i)).
        const auto &range = match[i];
        // b. Perform ! CreateDataPropertyOrThrow(A, ! ToString(lengthA),
        // nextCapture).
        if (!range) {
          if (LLVM_UNLIKELY(
                  JSArray::setElementAt(
                      lv.A, runtime, lengthA, Runtime::getUndefinedValue()) ==
                  ExecutionStatus::EXCEPTION))
            return ExecutionStatus::EXCEPTION;
        } else {
          if (LLVM_UNLIKELY(
                  (strRes = StringPrimitive::slice(
                       runtime, lv.S, range->location, range->length)) ==
                  ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          lv.captureStr.castAndSetHermesValue<StringPrimitive>(*strRes);
          if (LLVM_UNLIKELY(
                  JSArray::setElementAt(
                      lv.A, runtime, lengthA, lv.captureStr) ==
                  ExecutionStatus::EXCEPTION))
            return ExecutionStatus::EXCEPTION;
        }
        // d. Let lengthA be lengthA +1.
        ++lengthA;
        // e. If lengthA = lim, return A.
        if (lengthA == lim) {
          if (LLVM_UNLIKELY(
                  JSArray::setLengthProperty(lv.A, runtime, lengthA) ==
                  ExecutionStatus::EXCEPTION))
            return ExecutionStatus::EXCEPTION;
          return lv.A.getHermesValue();
        }
      }
      // 10. Set q to p.
      // Start position of the next search is updated to the end of this match.
      q = p;
    }
  }

  // 20. Let T be the String value equal to the substring of S consisting of the
  // code units at indices p (inclusive) through size (exclusive).
  // Add the rest of the string (after the last match) to A.
  auto elementStrRes = StringPrimitive::slice(runtime, lv.S, p, size - p);
  if (LLVM_UNLIKELY(elementStrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.tmpHandle = *elementStrRes;
  // 21. Perform ! CreateDataPropertyOrThrow(A, ! ToString(lengthA), T).
  if (LLVM_UNLIKELY(
          JSArray::setElementAt(lv.A, runtime, lengthA, lv.tmpHandle) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  ++lengthA;

  if (LLVM_UNLIKELY(
          JSArray::setLengthProperty(lv.A, runtime, lengthA) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  // 22. Return A.
  return lv.A.getHermesValue();
}

// ES2022 22.2.5.4
// Note that we don't yet support unicode.
CallResult<HermesValue> regExpFlagsGetter(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // Let R be the this value.
  // If Type(R) is not Object, throw a TypeError exception
  Handle<JSObject> R = args.dyncastThis<JSObject>();
  if (!R) {
    return runtime.raiseTypeError(
        "RegExp.prototype.flags getter called on non-object");
  }

  llvh::SmallString<7> result;
  static const struct FlagProp {
    char flagChar;
    Predefined::Str name;
  } flagProps[] = {
      {'d', Predefined::hasIndices},
      {'g', Predefined::global},
      {'i', Predefined::ignoreCase},
      {'m', Predefined::multiline},
      {'s', Predefined::dotAll},
      {'u', Predefined::unicode},
      {'y', Predefined::sticky},
  };
  for (FlagProp f : flagProps) {
    auto flagVal =
        JSObject::getNamed_RJS(R, runtime, Predefined::getSymbolID(f.name));
    if (LLVM_UNLIKELY(flagVal == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (toBoolean(flagVal->get())) {
      result.push_back(f.flagChar);
    }
  }
  return StringPrimitive::create(runtime, result);
}

} // namespace vm
} // namespace hermes

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
#pragma GCC diagnostic push

#ifdef HERMES_COMPILER_SUPPORTS_WSHORTEN_64_TO_32
#pragma GCC diagnostic ignored "-Wshorten-64-to-32"
#endif
namespace hermes {
namespace vm {

/// Internal methods
/// {@

/// 21.2.3.2.1 Runtime Semantics: RegExpAlloc ( newTarget )
static CallResult<HermesValue> regExpAlloc(
    Runtime &runtime,
    PseudoHandle<> /* newTarget */);

/// 21.2.3.2.2 Runtime Semantics: RegExpInitialize ( obj, pattern, flags )
static CallResult<Handle<JSRegExp>> regExpInitialize(
    Runtime &runtime,
    Handle<> obj,
    Handle<> pattern,
    Handle<> flags);

/// ES6.0 21.2.5.2.2
static CallResult<HermesValue> regExpBuiltinExec(
    Runtime &runtime,
    Handle<JSRegExp> R,
    Handle<StringPrimitive> S);

/// @}

// Several RegExp accessors are defined to do particular things when passed the
// RegExp object (which is not itself a RegExp). Centralize that logic here.
static inline bool thisIsRegExpProto(Runtime &runtime, NativeArgs args) {
  return args.dyncastThis<JSObject>().get() ==
      vmcast<JSObject>(runtime.regExpPrototype);
}

Handle<JSObject> createRegExpConstructor(Runtime &runtime) {
  auto proto = Handle<JSObject>::vmcast(&runtime.regExpPrototype);

  auto cons = defineSystemConstructor<JSRegExp>(
      runtime,
      Predefined::getSymbolID(Predefined::RegExp),
      regExpConstructor,
      proto,
      2,
      CellKind::JSRegExpKind);

  defineMethod(
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

  defineGetter(cons, Predefined::dollar1, regExpDollarNumberGetter, 1);
  defineGetter(cons, Predefined::dollar2, regExpDollarNumberGetter, 2);
  defineGetter(cons, Predefined::dollar3, regExpDollarNumberGetter, 3);
  defineGetter(cons, Predefined::dollar4, regExpDollarNumberGetter, 4);
  defineGetter(cons, Predefined::dollar5, regExpDollarNumberGetter, 5);
  defineGetter(cons, Predefined::dollar6, regExpDollarNumberGetter, 6);
  defineGetter(cons, Predefined::dollar7, regExpDollarNumberGetter, 7);
  defineGetter(cons, Predefined::dollar8, regExpDollarNumberGetter, 8);
  defineGetter(cons, Predefined::dollar9, regExpDollarNumberGetter, 9);
  defineGetter(cons, Predefined::leftContext, regExpLeftContextGetter);
  defineGetter(cons, Predefined::dollarBacktick, regExpLeftContextGetter);
  defineGetter(cons, Predefined::rightContext, regExpRightContextGetter);
  defineGetter(cons, Predefined::dollarApostrophe, regExpRightContextGetter);
  defineGetter(cons, Predefined::dollarUnderscore, regExpInputGetter);
  defineGetter(cons, Predefined::input, regExpInputGetter);
  defineGetter(cons, Predefined::dollarAmpersand, regExpLastMatchGetter);
  defineGetter(cons, Predefined::lastMatch, regExpLastMatchGetter);
  defineGetter(cons, Predefined::dollarPlus, regExpLastParenGetter);
  defineGetter(cons, Predefined::lastParen, regExpLastParenGetter);

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

  return cons;
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

/// 21.2.3.2.1 Runtime Semantics: RegExpAlloc ( newTarget )
// TODO: This is currently a wrapper around JSRegExp::create, and always creates
// an object with RegExp.prototype as its prototype. After supporting
// subclassing, this should be updated to allocate an object with its prototype
// set to newTarget.prototype.
static CallResult<HermesValue> regExpAlloc(
    Runtime &runtime,
    PseudoHandle<> /* newTarget */) {
  return JSRegExp::create(runtime).getHermesValue();
}

/// ES11 21.2.3.2.2 RegExpInitialize ( obj, pattern, flags ) 1-4
static CallResult<Handle<JSRegExp>> regExpInitialize(
    Runtime &runtime,
    Handle<> obj,
    Handle<> pattern,
    Handle<> flags) {
  auto regExpObj = Handle<JSRegExp>::dyn_vmcast(obj);
  if (LLVM_UNLIKELY(!regExpObj)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 1. If pattern is undefined, let P be the empty String.
  // 2. Else, let P be ? ToString(pattern).
  MutableHandle<StringPrimitive> P = runtime.makeMutableHandle(
      runtime.getPredefinedString(Predefined::emptyString));
  if (!pattern->isUndefined()) {
    auto strRes = toString_RJS(runtime, pattern);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    P = strRes->get();
  }
  // 3. If flags is undefined, let F be the empty String.
  // 4. Else, let F be ? ToString(flags).
  MutableHandle<StringPrimitive> F = runtime.makeMutableHandle(
      runtime.getPredefinedString(Predefined::emptyString));
  if (!flags->isUndefined()) {
    auto strRes = toString_RJS(runtime, flags);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    F = strRes->get();
  }
  if (JSRegExp::initialize(regExpObj, runtime, P, F) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return regExpObj;
}

/// ES6.0 21.2.3.2.3 Runtime Semantics: RegExpCreate ( P, F )
CallResult<Handle<JSRegExp>>
regExpCreate(Runtime &runtime, Handle<> P, Handle<> F) {
  auto objRes =
      regExpAlloc(runtime, createPseudoHandle(runtime.regExpPrototype));
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return regExpInitialize(runtime, runtime.makeHandle(objRes.getValue()), P, F);
}

/// ES6 21.2.3.1 RegExp ( pattern, flags )
/// The internals of \c regExpConstructor. It's extracted to allow internal call
/// sites such as @@MatchAll to construct a new JSRegExp via handles.
/// \p isConstructorCall true if a newTarget is not undefined.
// TODO: right now the NewTarget can only be the RegExp constructor itself,
// after supporting subclassing, this function should take the NewTarget as a
// param and compute \p isConstructorCall from the NewTarget.
static CallResult<Handle<JSObject>> regExpConstructorInternal(
    Runtime &runtime,
    Handle<> pattern,
    Handle<> flags,
    bool isConstructorCall) {
  // 1. Let patternIsRegExp be IsRegExp(pattern).
  // 2. ReturnIfAbrupt(patternIsRegExp).
  auto callRes = isRegExp(runtime, pattern);
  if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  bool patternIsRegExp = callRes.getValue();
  // 3. If NewTarget is not undefined, let newTarget be NewTarget.
  // Note: "NewTarget is not undefined" means we are in a constructor call.
  // TODO: right now the NewTarget can only be the RegExp constructor itself,
  // after supporting subclassing, NewTarget could be the constructor of the
  // derived class.
  auto propRes = JSObject::getNamed_RJS(
      runtime.getGlobal(),
      runtime,
      Predefined::getSymbolID(Predefined::RegExp));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<> newTarget = runtime.makeHandle(std::move(*propRes));
  // 4. Else,
  // This is not a constructor call.
  if (!isConstructorCall) {
    // a. Let newTarget be the active function object.
    // Note: newTarget is the RegExp constructor, which is already set.
    // b. If patternIsRegExp is true and flags is undefined, then
    if (patternIsRegExp && flags->isUndefined()) {
      // i. Let patternConstructor be Get(pattern, "constructor").
      // ii. ReturnIfAbrupt(patternConstructor).
      auto patternObj = Handle<JSObject>::vmcast(pattern);
      auto patternConstructor = JSObject::getNamed_RJS(
          patternObj,
          runtime,
          Predefined::getSymbolID(Predefined::constructor));
      if (LLVM_UNLIKELY(patternConstructor == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      // iii. If SameValue(newTarget, patternConstructor) is true, return
      // pattern.
      if (isSameValue(newTarget.getHermesValue(), patternConstructor->get())) {
        // Note: unfortunately, even at this point it's still unsafe to assume
        // pattern is a JSRegExp, see the objWithRegExpCons from
        // test/hermes/regexp.js for an example of how such assumptions falls.
        return patternObj;
      }
    }
  }

  MutableHandle<> P{runtime};
  MutableHandle<> F{runtime};
  // 5. If Type(pattern) is Object and pattern has a [[RegExpMatcher]] internal
  // slot, then
  if (auto patternAsRegExp = Handle<JSRegExp>::dyn_vmcast(pattern)) {
    // a. Let P be the value of pattern’s [[OriginalSource]] internal slot.
    P = JSRegExp::getPattern(patternAsRegExp.get(), runtime).getHermesValue();
    // b. If flags is undefined, let F be the value of pattern’s
    // [[OriginalFlags]] internal slot.
    if (flags->isUndefined()) {
      auto flagsStr =
          JSRegExp::getSyntaxFlags(patternAsRegExp.get()).toString();
      auto strRes = StringPrimitive::create(runtime, flagsStr);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      F = strRes.getValue();
    } else {
      // c. Else, let F be flags.
      F = flags.getHermesValue();
    }
  } else if (patternIsRegExp) {
    // 6. Else if patternIsRegExp is true, then
    //   a. Let P be Get(pattern, "source").
    //   b. ReturnIfAbrupt(P).
    Handle<JSObject> patternObj = Handle<JSObject>::vmcast(pattern);
    propRes = JSObject::getNamed_RJS(
        patternObj, runtime, Predefined::getSymbolID(Predefined::source));
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    P = std::move(propRes.getValue());
    //   c. If flags is undefined, then
    if (flags->isUndefined()) {
      //   i. Let F be Get(pattern, "flags").
      //   ii. ReturnIfAbrupt(F).
      propRes = JSObject::getNamed_RJS(
          patternObj, runtime, Predefined::getSymbolID(Predefined::flags));
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      F = std::move(propRes.getValue());
    } else {
      //   d. Else, let F be flags.
      F = flags.getHermesValue();
    }
  } else {
    // 7. Else,
    //   a. Let P be pattern.
    P = pattern.getHermesValue();
    //   b. Let F be flags.
    F = flags.getHermesValue();
  }
  // 8. Let O be RegExpAlloc(newTarget).
  // 9. ReturnIfAbrupt(O).
  auto objRes = regExpAlloc(runtime, newTarget);
  if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 10. Return RegExpInitialize(O, P, F).
  auto regExpRes =
      regExpInitialize(runtime, runtime.makeHandle(objRes.getValue()), P, F);
  if (LLVM_UNLIKELY(regExpRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return Handle<JSObject>::vmcast(*regExpRes);
}

/// ES6 21.2.3.1 RegExp ( pattern, flags )
// This is just a wrapper of \c regExpConstructorInternal to take NativeArgs.
CallResult<HermesValue>
regExpConstructor(void *, Runtime &runtime, NativeArgs args) {
  Handle<> pattern = args.getArgHandle(0);
  Handle<> flags = args.getArgHandle(1);
  auto regExpRes = regExpConstructorInternal(
      runtime, pattern, flags, args.isConstructorCall());
  if (LLVM_UNLIKELY(regExpRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return regExpRes->getHermesValue();
}

/// Wrapper for regExpConstructorInternal to implement a fast path for
/// Construct(%RegExp%, « pattern, flags ») and avoid a recompilation in the
/// common case where \p pattern is a JSRegExp with [[OriginalFlags]] equal to
/// \p flags.
static CallResult<Handle<JSRegExp>> regExpConstructorFastCopy(
    Runtime &runtime,
    Handle<> pattern,
    Handle<StringPrimitive> flags) {
  if (auto R = Handle<JSRegExp>::dyn_vmcast(pattern)) {
    auto newRegexp = runtime.makeHandle(JSRegExp::create(runtime));
    if (LLVM_UNLIKELY(
            JSRegExp::initialize(newRegexp, runtime, R, flags) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return newRegexp;
  }
  auto newRegexpRes = regExpConstructorInternal(runtime, pattern, flags, true);
  if (LLVM_UNLIKELY(newRegexpRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return Handle<JSRegExp>::vmcast(*newRegexpRes);
}

static void createGroupsObject(
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
    return;
  }

  // The `__proto__` property on the groups object is not special,
  // and does not affect the [[Prototype]] of the resulting groups object.
  // This means that the prototype of the resulting groups object is null.
  auto clazzHandle = runtime.makeHandle(mappingObj->getClass(runtime));
  auto groupsObjRes = JSObject::create(
      runtime, Runtime::makeNullHandle<JSObject>(), clazzHandle);
  auto groupsObj = runtime.makeHandle(groupsObjRes.get());

  HiddenClass::forEachProperty(
      clazzHandle, runtime, [&](SymbolID id, NamedPropertyDescriptor desc) {
        auto groupIdx =
            JSObject::getNamedSlotValueUnsafe(*mappingObj, runtime, desc.slot)
                .getNumber(runtime);
        JSObject::setNamedSlotValueUnsafe(
            *groupsObj, runtime, desc.slot, matchObj->at(runtime, groupIdx));
      });

  auto shv = SmallHermesValue::encodeObjectValue(*groupsObj, runtime);
  JSObject::setNamedSlotValueUnsafe(matchObj.get(), runtime, groupsDesc, shv);
}

/// ES2022 22.2.7.8 MakeMatchIndicesIndexPairArray
static CallResult<Handle<JSArray>> makeMatchIndicesIndexPairArray(
    Runtime &runtime,
    Handle<StringPrimitive> S,
    RegExpMatch indices,
    Handle<JSObject> mappingObj,
    bool hasGroups) {
  // 1. Let n be the number of elements in indices.
  size_t n = indices.size();
  // 2-4 skipped because mappingObj is not list, but an object in our
  // implementation.
  // 5. Let A be ! ArrayCreate(n).
  auto arrRes = JSArray::create(runtime, n, n);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<JSArray> A = runtime.makeHandle<JSArray>(*arrRes);
  JSArray::setStorageEndIndex(A, runtime, indices.size());

  // 6-8. done later. We can't code exactly to spec here because mappingObj
  // (roughly groupNames in the spec) is not an array, but an object containing
  // group name and index.

  MutableHandle<> matchIndexPair{runtime};
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
      auto pair = runtime.makeHandle<JSArray>(*matchIndexPairRes);
      JSArray::setStorageEndIndex(pair, runtime, 2);
      auto firstIdx =
          SmallHermesValue::encodeNumberValue(matchIndices->location, runtime);
      JSArray::unsafeSetExistingElementAt(*pair, runtime, 0, firstIdx);
      auto secondIdx = SmallHermesValue::encodeNumberValue(
          matchIndices->location + matchIndices->length, runtime);
      JSArray::unsafeSetExistingElementAt(*pair, runtime, 1, secondIdx);
      matchIndexPair = pair.getHermesValue();
    } else {
      // c. Else,
      // i. Let matchIndexPair be undefined.
      matchIndexPair = Runtime::getUndefinedValue();
    }
    // d. Perform ! CreateDataPropertyOrThrow(A, ! ToString(𝔽(i)),
    // matchIndexPair).
    auto shv = SmallHermesValue::encodeHermesValue(*matchIndexPair, runtime);
    JSArray::unsafeSetExistingElementAt(*A, runtime, i, shv);
  }

  // This is done out of order. See note above.
  // 6. If hasGroups is true, then
  MutableHandle<> groups{runtime};
  if (hasGroups) {
    // a. Let groups be OrdinaryObjectCreate(null).
    auto mappingObjClazz = runtime.makeHandle(mappingObj->getClass(runtime));
    auto groupsRes = JSObject::create(
        runtime, Runtime::makeNullHandle<JSObject>(), mappingObjClazz);
    auto groupsObj = runtime.makeHandle(groupsRes.get());
    HiddenClass::forEachProperty(
        mappingObjClazz,
        runtime,
        [&](SymbolID id, NamedPropertyDescriptor desc) {
          auto groupIdx =
              JSObject::getNamedSlotValueUnsafe(*mappingObj, runtime, desc.slot)
                  .getNumber(runtime);
          // 9.e. If i > 0 and groupNames[i - 1] is not undefined, then
          // ii. Perform ! CreateDataPropertyOrThrow(groups, groupNames[i-1],
          // matchIndexPair).
          JSObject::setNamedSlotValueUnsafe(
              *groupsObj, runtime, desc.slot, A->at(runtime, groupIdx));
        });
    groups = groupsObj.getHermesValue();
  } else {
    // 7. Else,
    // a. Let groups be undefined.
    groups = Runtime::getUndefinedValue();
  }

  // 8. Perform ! CreateDataPropertyOrThrow(A, "groups", groups).
  if (LLVM_UNLIKELY(
          JSObject::defineOwnProperty(
              A,
              runtime,
              Predefined::getSymbolID(Predefined::groups),
              DefinePropertyFlags::getDefaultNewPropertyFlags(),
              groups) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // 10. Return A.
  return A;
}

// ES6 21.2.5.2.2
CallResult<Handle<JSArray>> directRegExpExec(
    Handle<JSRegExp> regexp,
    Runtime &runtime,
    Handle<StringPrimitive> S) {
  MutableHandle<JSArray> A{runtime};
  GCScope gcScope{runtime};

  // Let length be the number of code units in S.
  const uint32_t length = S->getStringLength();

  // Let lastIndex be ? ToLength(? Get(R, "lastIndex")).
  auto propRes = runtime.getNamed(regexp, PropCacheID::RegExpLastIndex);
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto lengthRes =
      toLengthU64(runtime, runtime.makeHandle(std::move(*propRes)));
  if (LLVM_UNLIKELY(lengthRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  uint64_t lastIndex = *lengthRes;

  // Let flags be R.[[OriginalFlags]]
  const regex::SyntaxFlags flags = JSRegExp::getSyntaxFlags(regexp.get());

  // If flags contains "g", let global be true, else let global be false
  // If flags contains "y", let sticky be true, else let sticky be false.
  // If flags contains "d", let hasIndices be true, else let hasIndices be
  // false.
  // If flags contains "u", let fullUnicode be true, else let fullUnicode be
  // false.
  const bool global = flags.global;
  const bool sticky = flags.sticky;
  const bool hasIndices = flags.hasIndices;
  const bool fullUnicode = flags.unicode;

  // If global is false and sticky is false, set lastIndex to 0.
  if (!global && !sticky) {
    lastIndex = 0;
  }

  // The ES6 spec is cast in terms of a loop on "matchSucceeded". This is
  // handled internally by JSRegExp::search for efficiency reasons.
  // (Note that lastIndex == length is allowed: it may match an empty range at
  // the end of the string.)
  CallResult<RegExpMatch> matchResult = RegExpMatch{};
  if (lastIndex <= length) {
    assert(
        lastIndex <= UINT32_MAX &&
        "lastIndex should fit in u32 because it is <= a string length");
    uint32_t searchStartOffset = static_cast<uint32_t>(lastIndex);
    matchResult = JSRegExp::search(regexp, runtime, S, searchStartOffset);
  }

  if (LLVM_UNLIKELY(matchResult == ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  RegExpMatch &match = *matchResult;
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
    return Runtime::makeNullHandle<JSArray>();
  }

  // We have a match!
  // "If global is true or sticky is true, Let setStatus be Set(R, "lastIndex",
  // e, true)."
  // Here 'e' is the end of the total match.
  assert(!match.empty() && "Match should not be empty");

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

  if (global || sticky) {
    auto totalMatch = *match.front();
    uint32_t e = totalMatch.location + totalMatch.length;
    if (LLVM_UNLIKELY(
            setLastIndex(regexp, runtime, e) == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }

  auto arrRes = JSArray::createAndAllocPropStorage(
      runtime,
      Handle<JSObject>::vmcast(&runtime.arrayPrototype),
      Handle<HiddenClass>::vmcast(&runtime.regExpMatchClass),
      match.size(),
      match.size());
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  A = arrRes->get();

  // A already has .index and .groups.
  NamedPropertyDescriptor indexDesc;
  bool res = JSObject::getOwnNamedDescriptor(
      A, runtime, Predefined::getSymbolID(Predefined::index), indexDesc);
  assert(res && "match object is missing .index property");
  (void)res;
  auto indexSHV =
      SmallHermesValue::encodeNumberValue(match.front()->location, runtime);
  JSObject::setNamedSlotValueUnsafe(*A, runtime, indexDesc, indexSHV);

  NamedPropertyDescriptor inputDesc;
  res = JSObject::getOwnNamedDescriptor(
      A, runtime, Predefined::getSymbolID(Predefined::input), inputDesc);
  assert(res && "match object is missing .input property");
  (void)res;
  auto inputSHV = SmallHermesValue::encodeStringValue(*S, runtime);
  JSObject::setNamedSlotValueUnsafe(*A, runtime, inputDesc, inputSHV);

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
      JSArray::setElementAt(A, runtime, idx, Runtime::getUndefinedValue());
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
      JSArray::setElementAt(
          A, runtime, idx, runtime.makeHandle<StringPrimitive>(*strRes));
    }
    idx++;
  }

  // If hasIndices is true, then
  if (hasIndices) {
    // Let indicesArray be MakeMatchIndicesIndexPairArray(S, indices,
    // groupNames, hasGroups).
    auto indicesArray = makeMatchIndicesIndexPairArray(
        runtime, S, match, groupNames, hasGroups);
    if (LLVM_UNLIKELY(indicesArray == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // Perform ! CreateDataPropertyOrThrow(A, "indices", indicesArray).
    if (LLVM_UNLIKELY(
            JSObject::defineOwnProperty(
                A,
                runtime,
                Predefined::getSymbolID(Predefined::indices),
                DefinePropertyFlags::getDefaultNewPropertyFlags(),
                *indicesArray) == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
  }

  createGroupsObject(runtime, A, groupNames);
  return A;
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
  CallResult<Handle<JSArray>> result = directRegExpExec(R, runtime, S);
  if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // When the match is empty, result is a nullptr handle, which means
  // result.getValue().getHermesValue() is not HermesValue::encodeNullValue()
  // but HermesValue::encodeObjectValue(nullptr).
  if (!result.getValue())
    return HermesValue::encodeNullValue();
  return result->getHermesValue();
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

/// ES6.0 21.2.5.2.1 Runtime Semantics: RegExpExec ( R, S )
CallResult<HermesValue>
regExpExec(Runtime &runtime, Handle<JSObject> R, Handle<StringPrimitive> S) {
  // 3. Let exec be Get(R, "exec").
  // 4. ReturnIfAbrupt(exec).
  auto propRes = JSObject::getNamed_RJS(
      R, runtime, Predefined::getSymbolID(Predefined::exec));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto exec = runtime.makeHandle(std::move(propRes.getValue()));
  // 5. If IsCallable(exec) is true, then
  if (auto execCallable = Handle<Callable>::dyn_vmcast(exec)) {
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
CallResult<HermesValue>
regExpPrototypeExec(void *, Runtime &runtime, NativeArgs args) {
  Handle<JSRegExp> regexp = args.dyncastThis<JSRegExp>();
  if (!regexp) {
    return runtime.raiseTypeError(
        "RegExp function called on non-RegExp object");
  }

  auto strRes = toString_RJS(runtime, args.getArgHandle(0));
  if (strRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<StringPrimitive> S = runtime.makeHandle(std::move(*strRes));
  CallResult<Handle<JSArray>> result = directRegExpExec(regexp, runtime, S);
  if (LLVM_UNLIKELY(result.getStatus() == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // When the match is empty, result is a nullptr handle, which means
  // result.getValue().getHermesValue() is not HermesValue::encodeNullValue()
  // but HermesValue::encodeObjectValue(nullptr).
  if (!result.getValue())
    return HermesValue::encodeNullValue();
  return result.getValue().getHermesValue();
}

/// Implementation of RegExp.prototype.test
/// Returns true if a match is found, false otherwise
/// TODO optimization: avoid constructing the submatch array. Instead simply
/// check for a match.
CallResult<HermesValue>
regExpPrototypeTest(void *context, Runtime &runtime, NativeArgs args) {
  CallResult<HermesValue> res = regExpPrototypeExec(context, runtime, args);
  if (res == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;

  return HermesValue::encodeBoolValue(
      !runtime.makeHandle(res.getValue())->isNull());
}

/// Return the ith capture group in the most recent successful RegExp search.
/// If there was no ith capture group, return "".
CallResult<HermesValue>
regExpDollarNumberGetter(void *ctx, Runtime &runtime, NativeArgs args) {
  size_t i = reinterpret_cast<size_t>(ctx);

  auto match = runtime.regExpLastMatch;
  if (match.size() >= i + 1 &&
      vmisa<StringPrimitive>(runtime.regExpLastInput)) {
    const auto &cap = match[i];
    if (cap.hasValue()) {
      auto S = Handle<StringPrimitive>::vmcast(&runtime.regExpLastInput);
      auto strRes =
          StringPrimitive::slice(runtime, S, cap->location, cap->length);
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
CallResult<HermesValue>
regExpSourceGetter(void *ctx, Runtime &runtime, NativeArgs args) {
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
  return JSRegExp::escapePattern(
      runtime.makeHandle(JSRegExp::getPattern(R.get(), runtime)), runtime);
}

// ES8 21.2.5.4, 21.2.5.5, 21.2.5.7
CallResult<HermesValue>
regExpFlagPropertyGetter(void *ctx, Runtime &runtime, NativeArgs args) {
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

CallResult<HermesValue>
regExpLeftContextGetter(void *ctx, Runtime &runtime, NativeArgs args) {
  auto match = runtime.regExpLastMatch;
  if (match.size() >= 1 && vmisa<StringPrimitive>(runtime.regExpLastInput)) {
    auto S = Handle<StringPrimitive>::vmcast(&runtime.regExpLastInput);
    auto strRes = StringPrimitive::slice(runtime, S, 0, match[0]->location);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return *strRes;
  }

  return HermesValue::encodeStringValue(
      runtime.getPredefinedString(Predefined::emptyString));
}

CallResult<HermesValue>
regExpRightContextGetter(void *ctx, Runtime &runtime, NativeArgs args) {
  auto match = runtime.regExpLastMatch;
  if (match.size() >= 1 && vmisa<StringPrimitive>(runtime.regExpLastInput)) {
    auto S = Handle<StringPrimitive>::vmcast(&runtime.regExpLastInput);
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

CallResult<HermesValue>
regExpInputGetter(void *ctx, Runtime &runtime, NativeArgs args) {
  if (vmisa<StringPrimitive>(runtime.regExpLastInput)) {
    return runtime.regExpLastInput;
  }

  return HermesValue::encodeStringValue(
      runtime.getPredefinedString(Predefined::emptyString));
}

CallResult<HermesValue>
regExpLastMatchGetter(void *ctx, Runtime &runtime, NativeArgs args) {
  auto match = runtime.regExpLastMatch;
  if (match.size() >= 1 && vmisa<StringPrimitive>(runtime.regExpLastInput)) {
    auto S = Handle<StringPrimitive>::vmcast(&runtime.regExpLastInput);
    auto strRes = StringPrimitive::slice(
        runtime, S, match[0]->location, match[0]->length);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return *strRes;
  }

  return HermesValue::encodeStringValue(
      runtime.getPredefinedString(Predefined::emptyString));
}

CallResult<HermesValue>
regExpLastParenGetter(void *ctx, Runtime &runtime, NativeArgs args) {
  auto match = runtime.regExpLastMatch;
  if (match.size() >= 2 && vmisa<StringPrimitive>(runtime.regExpLastInput)) {
    auto S = Handle<StringPrimitive>::vmcast(&runtime.regExpLastInput);
    const auto &cap = match.back();
    if (cap.hasValue()) {
      auto strRes =
          StringPrimitive::slice(runtime, S, cap->location, cap->length);
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
        return StringPrimitive::createStringView(
            runtime,
            runtime.makeHandle<StringPrimitive>(
                captures->at(idx).getString(runtime)));
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
        auto identifier =
            replacementView.slice(i + 2, gtPos - (i + 2)).getUTF16Ref(storage);
        auto symbolRes =
            runtime.getIdentifierTable().getSymbolHandle(runtime, identifier);
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
          auto toStrRes =
              toString_RJS(runtime, runtime.makeHandle(std::move(*captureRes)));
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
CallResult<HermesValue>
regExpPrototypeSymbolMatchAll(void *, Runtime &runtime, NativeArgs args) {
  GCScope gcScope{runtime};

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
  auto S = runtime.makeHandle(std::move(*strRes));

  // 4. Let C be ? SpeciesConstructor(R, %RegExp%).
  // Since species constructors is not supported, C is always %RegExp%.

  // 5. Let flags be ? ToString(? Get(R, "flags")).
  auto flagsPropRes = JSObject::getNamed_RJS(
      R, runtime, Predefined::getSymbolID(Predefined::flags));
  if (LLVM_UNLIKELY(flagsPropRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto flagsStrRes =
      toString_RJS(runtime, runtime.makeHandle(std::move(*flagsPropRes)));
  if (LLVM_UNLIKELY(flagsStrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto flags = runtime.makeHandle(std::move(*flagsStrRes));

  // 6. Let matcher be ? Construct(C, « R, flags »).
  // This is thus equivalent to RegExp(« R, flags »).
  // it is necessary to invoke the 21.2.3.1 RegExp, neither RegExpCreate
  // nor internal JSRegExp creation can shortcut without diverging the spec.
  auto newRegExpRes = regExpConstructorFastCopy(runtime, R, flags);
  if (newRegExpRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto matcher = *newRegExpRes;

  // 7. Let lastIndex be ? ToLength(? Get(R, "lastIndex")).
  auto propRes = JSObject::getNamed_RJS(
      R, runtime, Predefined::getSymbolID(Predefined::lastIndex));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto lenRes = toLength(runtime, runtime.makeHandle(std::move(*propRes)));
  if (LLVM_UNLIKELY(lenRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  double lastIndex = lenRes->getNumber();
  // 8. Perform ? Set(matcher, "lastIndex", lastIndex, true).
  if (setLastIndex(matcher, runtime, lastIndex) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  // 9. If flags contains "g", let global be true.
  // 10. Else, let global be false.
  // 11. If flags contains "u", let fullUnicode be true.
  // 12. Else, let fullUnicode be false.
  bool global = false;
  bool fullUnicode = false;
  auto strView = StringPrimitive::createStringView(runtime, flags);
  for (auto it = strView.begin(); it < strView.end(); it++) {
    if (*it == 'g')
      global = true;
    if (*it == 'u')
      fullUnicode = true;
  }

  // 13. Return ! CreateRegExpStringIterator(matcher, S, global, fullUnicode).
  return JSRegExpStringIterator::create(
             runtime, matcher, S, global, fullUnicode)
      .getHermesValue();
}

// ES6 21.2.5.14
// Note there is no requirement that 'this' be a RegExp object.
CallResult<HermesValue>
regExpPrototypeToString(void *, Runtime &runtime, NativeArgs args) {
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
  auto patternRes =
      toString_RJS(runtime, runtime.makeHandle(std::move(*source)));
  if (LLVM_UNLIKELY(patternRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<StringPrimitive> pattern = runtime.makeHandle(std::move(*patternRes));

  // Let flags be ToString(Get(R, "flags"))
  auto flagsObj = JSObject::getNamed_RJS(
      regexp, runtime, Predefined::getSymbolID(Predefined::flags));
  if (LLVM_UNLIKELY(flagsObj == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto flagsRes =
      toString_RJS(runtime, runtime.makeHandle(std::move(*flagsObj)));
  if (LLVM_UNLIKELY(flagsRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<StringPrimitive> flags = runtime.makeHandle(std::move(*flagsRes));

  // 'Let result be the String value formed by concatenating "/", pattern, and
  // "/", and flags.' We expect 2 slashes plus at most 5 flags.
  SmallU16String<32> result;
  result.reserve(pattern->getStringLength() + 2 + 5);

  result.push_back(u'/');
  pattern->appendUTF16String(result);
  result.push_back(u'/');
  flags->appendUTF16String(result);
  return StringPrimitive::create(runtime, result);
}

// TODO: consider writing this in JS.
/// ES6.0 21.2.5.6
CallResult<HermesValue>
regExpPrototypeSymbolMatch(void *, Runtime &runtime, NativeArgs args) {
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
  auto S = runtime.makeHandle(std::move(*strRes));
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
    return regExpExec(runtime, rx, S);
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
  auto A = *arrRes;
  // e. Let n be 0.
  uint32_t n = 0;

  // g. Repeat,
  MutableHandle<> propValue{runtime};
  MutableHandle<> result{runtime};
  MutableHandle<StringPrimitive> matchStr{runtime};
  auto marker = gcScope.createMarker();
  while (true) {
    gcScope.flushToMarker(marker);
    // i. Let result be RegExpExec(rx, S).
    auto callRes = regExpExec(runtime, rx, S);
    // ii. ReturnIfAbrupt(result).
    if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    result = callRes.getValue();
    // iii. If result is null, then
    if (result->isNull()) {
      if (n == 0) {
        return HermesValue::encodeNullValue();
      } else {
        if (LLVM_UNLIKELY(
                JSArray::setLengthProperty(A, runtime, n) ==
                ExecutionStatus::EXCEPTION))
          return ExecutionStatus::EXCEPTION;
        return A.getHermesValue();
      }
    }
    // iv. Else result is not null,
    auto resultObj = Handle<JSObject>::vmcast(result);
    // 1. Let matchStr be ToString(Get(result, "0")).
    // 2. ReturnIfAbrupt(matchStr).
    auto propRes2 = JSObject::getComputed_RJS(
        resultObj, runtime, HandleRootOwner::getZeroValue());
    if (propRes2 == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    propValue = std::move(propRes2.getValue());
    auto strRes2 = toString_RJS(runtime, propValue);
    if (strRes2 == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    matchStr = strRes2->get();
    // 3. Let status be CreateDataProperty(A, ToString(n), matchStr).
    // 4. Assert: status is true.
    JSArray::setElementAt(A, runtime, n, matchStr);
    // 5. If matchStr is the empty String, then
    if (matchStr->getStringLength() == 0) {
      // a. Let thisIndex be ToLength(Get(rx, "lastIndex")).
      // b. ReturnIfAbrupt(thisIndex).
      if ((propRes = runtime.getNamed(rx, PropCacheID::RegExpLastIndex)) ==
          ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      propValue = std::move(propRes.getValue());
      auto thisIndex = toLength(runtime, propValue);
      if (thisIndex == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      // c. Let nextIndex be AdvanceStringIndex(S, thisIndex, fullUnicode).
      double nextIndex = advanceStringIndex(
          S.get(), thisIndex->getNumberAs<uint64_t>(), fullUnicode);
      // d. Let setStatus be Set(rx, "lastIndex", nextIndex, true).
      auto setStatus = setLastIndex(rx, runtime, nextIndex);
      // e. ReturnIfAbrupt(setStatus).
      if (setStatus == ExecutionStatus::EXCEPTION) {
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
CallResult<HermesValue>
regExpPrototypeSymbolSearch(void *, Runtime &runtime, NativeArgs args) {
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
  auto S = runtime.makeHandle(std::move(*strRes));
  // 5. Let previousLastIndex be Get(rx, "lastIndex").
  // 6. ReturnIfAbrupt(previousLastIndex).
  auto propRes = runtime.getNamed(rx, PropCacheID::RegExpLastIndex);
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<> previousLastIndex =
      runtime.makeHandle(std::move(propRes.getValue()));
  // 7. Let status be Set(rx, "lastIndex", 0, true).
  auto status = setLastIndex(rx, runtime, 0);
  // 8. ReturnIfAbrupt(status).
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 9. Let result be RegExpExec(rx, S).
  // 10. ReturnIfAbrupt(result).
  auto execRes = regExpExec(runtime, rx, S);
  if (LLVM_UNLIKELY(execRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<> result = runtime.makeHandle(execRes.getValue());
  // 11. Let status be Set(rx, "lastIndex", previousLastIndex, true).
  auto previousLastIndexSHV =
      SmallHermesValue::encodeHermesValue(*previousLastIndex, runtime);
  status = setLastIndex(rx, runtime, previousLastIndexSHV);
  // 12. ReturnIfAbrupt(status).
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // 13. If result is null, return –1.
  if (result->isNull()) {
    return HermesValue::encodeUntrustedNumberValue(-1);
  }
  // 14. Return Get(result, "index").
  auto resultObj = Handle<JSObject>::dyn_vmcast(result);
  if (LLVM_UNLIKELY(!resultObj)) {
    return ExecutionStatus::EXCEPTION;
  }
  return JSObject::getNamed_RJS(
             resultObj, runtime, Predefined::getSymbolID(Predefined::index))
      .toCallResultHermesValue();
}

/// ES6.0 21.2.5.8
CallResult<HermesValue>
regExpPrototypeSymbolReplace(void *, Runtime &runtime, NativeArgs args) {
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
  auto S = runtime.makeHandle(std::move(*strRes));
  // 5. Let lengthS be the number of code unit elements in S.
  uint32_t lengthS = S->getStringLength();
  // 6. Let functionalReplace be IsCallable(replaceValue).
  Handle<> replaceValue = args.getArgHandle(1);
  MutableHandle<StringPrimitive> replaceValueStr{runtime};
  auto replaceFn = Handle<Callable>::dyn_vmcast(replaceValue);
  // 7. If functionalReplace is false, then
  if (!replaceFn) {
    //   a. Let replaceValue be ToString(replaceValue).
    //   b. ReturnIfAbrupt(replaceValue).
    auto strRes = toString_RJS(runtime, replaceValue);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    replaceValueStr = strRes->get();
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
  // 11. Let results be a new empty List.
  auto arrRes = ArrayStorageSmall::create(runtime, 16 /* capacity */);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  MutableHandle<ArrayStorageSmall> resultsHandle{
      runtime, vmcast<ArrayStorageSmall>(arrRes.getValue())};

  // 12. Let done be false.
  bool done = false;

  // 13. Repeat, while done is false
  MutableHandle<> propValue{runtime};
  MutableHandle<StringPrimitive> matchStr{runtime};
  MutableHandle<JSObject> result{runtime};
  auto stringView = StringPrimitive::createStringView(runtime, S);
  while (!done) {
    GCScopeMarkerRAII marker{runtime};
    // a. Let result be RegExpExec(rx, S).
    // b. ReturnIfAbrupt(result).
    auto execRes = regExpExec(runtime, rx, S);
    if (LLVM_UNLIKELY(execRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // c. If result is null, set done to true.
    if (execRes->isNull()) {
      done = true;
      break;
    }
    // d. Else result is not null,
    // If result is not null, it must be an object.
    result = vmcast<JSObject>(execRes.getValue());
    // i. Append result to the end of results.
    if (LLVM_UNLIKELY(
            resultsHandle->size() == ArrayStorageSmall::maxElements())) {
      return runtime.raiseRangeError("Out of memory for regexp results.");
    }
    if (LLVM_UNLIKELY(
            ArrayStorageSmall::push_back(resultsHandle, runtime, result) ==
            ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // ii. If global is false, set done to true.
    if (!global) {
      done = true;
    } else {
      // iii. Else,
      // 1. Let matchStr be ToString(Get(result, "0")).
      // 2. ReturnIfAbrupt(matchStr).
      propRes = JSObject::getComputed_RJS(
          result, runtime, HandleRootOwner::getZeroValue());
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      propValue = propRes->get();
      auto strRes = toString_RJS(runtime, propValue);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      matchStr = strRes->get();
      // 3. If matchStr is the empty String, then
      if (matchStr->getStringLength() == 0) {
        // a. Let thisIndex be ToLength(Get(rx, "lastIndex")).
        // b. ReturnIfAbrupt(thisIndex).
        if ((propRes = runtime.getNamed(rx, PropCacheID::RegExpLastIndex)) ==
            ExecutionStatus::EXCEPTION) {
          return ExecutionStatus::EXCEPTION;
        }
        propValue = std::move(propRes.getValue());
        auto thisIndex = toLength(runtime, propValue);
        if (thisIndex == ExecutionStatus::EXCEPTION) {
          return ExecutionStatus::EXCEPTION;
        }
        // c. Let nextIndex be AdvanceStringIndex(S, thisIndex, fullUnicode).
        double nextIndex = advanceStringIndex(
            S.get(), thisIndex->getNumberAs<uint64_t>(), fullUnicode);
        // d. Let setStatus be Set(rx, "lastIndex", nextIndex, true).
        auto setStatus = setLastIndex(rx, runtime, nextIndex);
        // e. ReturnIfAbrupt(setStatus).
        if (setStatus == ExecutionStatus::EXCEPTION) {
          return ExecutionStatus::EXCEPTION;
        }
      }
    }
  }

  // 14. Let accumulatedResult be the empty String value.
  SmallU16String<32> accumulatedResult{};
  // 15. Let nextSourcePosition be 0.
  uint32_t nextSourcePosition = 0;
  // 16. Repeat, for each result in results,
  MutableHandle<> valueHandle{runtime};
  MutableHandle<> nHandle{runtime};
  for (uint32_t i = 0, size = resultsHandle->size(); i < size; ++i) {
    GCScopeMarkerRAII marker1{runtime};
    result = vmcast<JSObject>(resultsHandle->at(i).getObject(runtime));
    // a. Let nCaptures be ToLength(Get(result, "length")).
    // b. ReturnIfAbrupt(nCaptures).
    propRes = JSObject::getNamed_RJS(
        result, runtime, Predefined::getSymbolID(Predefined::length));
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    valueHandle = std::move(propRes.getValue());
    auto lengthRes = toLength(runtime, valueHandle);
    if (LLVM_UNLIKELY(lengthRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // Use uint64_t so it can fit the result of toLength.
    uint64_t nCaptures = lengthRes->getNumberAs<uint64_t>();
    // c. Let nCaptures be max(nCaptures − 1, 0).
    nCaptures = nCaptures > 0 ? nCaptures - 1 : 0;
    // d. Let matched be ToString(Get(result, "0")).
    // e. ReturnIfAbrupt(matched).
    propRes = JSObject::getComputed_RJS(
        result, runtime, HandleRootOwner::getZeroValue());
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto strRes = toString_RJS(
        runtime, runtime.makeHandle(std::move(propRes.getValue())));
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto matched = runtime.makeHandle(std::move(*strRes));
    // f. Let matchLength be the number of code units in matched.
    uint32_t matchLength = matched->getStringLength();
    // g. Let position be ToIntegerOrInfinity(Get(result, "index")).
    // h. ReturnIfAbrupt(position).
    propRes = JSObject::getNamed_RJS(
        result, runtime, Predefined::getSymbolID(Predefined::index));
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto intRes = toIntegerOrInfinity(
        runtime, runtime.makeHandle(std::move(propRes.getValue())));
    if (LLVM_UNLIKELY(intRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // position could potentially be negative or Infinity here, so use double.
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
    if (LLVM_UNLIKELY(nCaptures > ArrayStorageSmall::maxElements())) {
      return runtime.raiseRangeError("Out of memory for capture groups.");
    }
    arrRes = ArrayStorageSmall::create(runtime, nCaptures);
    if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    MutableHandle<ArrayStorageSmall> capturesHandle{
        runtime, vmcast<ArrayStorageSmall>(arrRes.getValue())};
    MutableHandle<> capN{runtime};
    // l. Repeat while n ≤ nCaptures
    while (n <= nCaptures) {
      GCScopeMarkerRAII marker2{runtime};
      // i. Let capN be Get(result, ToString(n)).
      // ii. ReturnIfAbrupt(capN).
      nHandle = HermesValue::encodeUntrustedNumberValue(n);
      propRes = JSObject::getComputed_RJS(result, runtime, nHandle);
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      capN = std::move(propRes.getValue());
      // iii. If capN is not undefined, then
      if (!capN->isUndefined()) {
        // 1. Let capN be ToString(capN).
        // 2. ReturnIfAbrupt(capN).
        auto strRes = toString_RJS(runtime, capN);
        if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        capN = strRes->getHermesValue();
      }
      // iv. Append capN as the last element of captures.
      if (LLVM_UNLIKELY(
              ArrayStorageSmall::push_back(capturesHandle, runtime, capN) ==
              ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      // v. Let n be n+1
      n++;
    }

    // j. Let namedCaptures be ? Get(result, "groups").
    MutableHandle<HermesValue> namedCaptures{runtime};
    bool hasNamedCaptures = false;
    auto namedCapturesRes = JSObject::getNamed_RJS(
        result, runtime, Predefined::getSymbolID(Predefined::groups));
    if (LLVM_UNLIKELY(namedCapturesRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (!(*namedCapturesRes)->isUndefined()) {
      namedCaptures.set(namedCapturesRes->get());
      hasNamedCaptures = true;
    }

    // m. If functionalReplace is true, then
    MutableHandle<StringPrimitive> replacement{runtime};
    if (replaceFn) {
      CallResult<PseudoHandle<>> callRes{ExecutionStatus::EXCEPTION};
      {
        // i. Let replacerArgs be «matched».
        // Arguments: matched, captures, position, S (, groups).
        size_t replacerArgsCount = 1 + nCaptures + 2 + hasNamedCaptures;
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
        for (; argIdx <= capturesHandle->size(); ++argIdx) {
          newFrame->getArgRef(argIdx) =
              capturesHandle->at(argIdx - 1).unboxToHV(runtime);
        }
        // iii. Append position and S.
        newFrame->getArgRef(argIdx++) =
            HermesValue::encodeUntrustedNumberValue(position);
        newFrame->getArgRef(argIdx++) = S.getHermesValue();
        // iv. If namedCaptures is not undefined, then
        if (hasNamedCaptures) {
          newFrame->getArgRef(argIdx++) = *namedCaptures;
        }
        // v. Let replValue be Call(replaceValue, undefined, replacerArgs).
        callRes = Callable::call(replaceFn, runtime);
        if (callRes == ExecutionStatus::EXCEPTION) {
          return ExecutionStatus::EXCEPTION;
        }
      }
      // vi. Let replacement be ToString(replValue).
      auto strRes = toString_RJS(
          runtime, runtime.makeHandle(std::move(callRes.getValue())));
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      replacement = strRes->get();
    } else {
      // n. Else,
      // i. If namedCaptures is not undefined, then
      if (hasNamedCaptures) {
        auto objRes = toObject(runtime, namedCaptures);
        if (LLVM_UNLIKELY(objRes == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        // 1. Set namedCaptures to ? ToObject(namedCaptures).
        namedCaptures.set(*objRes);
      }

      // ii. Let replacement be GetSubstitution(matched, S, position, captures,
      // replaceValue).
      auto callRes = getSubstitution(
          runtime,
          matched,
          S,
          position,
          capturesHandle,
          hasNamedCaptures ? Handle<JSObject>::vmcast(namedCaptures)
                           : Runtime::makeNullHandle<JSObject>(),
          replaceValueStr);
      if (LLVM_UNLIKELY(callRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      replacement = vmcast<StringPrimitive>(callRes.getValue());
    }
    // o. ReturnIfAbrupt(replacement).
    // p. If position ≥ nextSourcePosition, then
    if (position >= nextSourcePosition) {
      // i. NOTE position should not normally move backwards. If it does, it is
      // an indication of an ill-behaving RegExp subclass or use of an access
      // triggered side-effect to change the global flag or other
      // characteristics of rx. In such cases, the corresponding substitution is
      // ignored.
      // ii. Let accumulatedResult be the String formed by
      // concatenating the code units of the current value of accumulatedResult
      // with the substring of S consisting of the code units from
      // nextSourcePosition (inclusive) up to position (exclusive) and with the
      // code units of replacement.
      stringView.slice(nextSourcePosition, position - nextSourcePosition)
          .appendUTF16String(accumulatedResult);
      replacement->appendUTF16String(accumulatedResult);
      // iii. Let nextSourcePosition be position + matchLength.
      nextSourcePosition = position + matchLength;
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
CallResult<HermesValue>
regExpPrototypeSymbolSplit(void *, Runtime &runtime, NativeArgs args) {
  GCScope gcScope{runtime};

  // 2. If Type(rx) is not Object, throw a TypeError exception.
  if (LLVM_UNLIKELY(!vmisa<JSObject>(args.getThisArg()))) {
    return runtime.raiseTypeError(
        "Cannot call RegExp.prototype[Symbol.split] on a non-object.");
  }
  // 3. Let S be ToString(string).
  auto strRes = toString_RJS(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto S = runtime.makeHandle(std::move(*strRes));

  // 5. Let flags be ? ToString(? Get(rx, "flags")).
  auto regexp = Handle<JSObject>::vmcast(args.getThisHandle());
  CallResult<PseudoHandle<>> flagsRes = JSObject::getNamed_RJS(
      regexp, runtime, Predefined::getSymbolID(Predefined::flags));
  if (LLVM_UNLIKELY(flagsRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  CallResult<PseudoHandle<StringPrimitive>> flagsStrRes =
      toString_RJS(runtime, runtime.makeHandle(std::move(flagsRes.getValue())));
  if (LLVM_UNLIKELY(flagsStrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  Handle<StringPrimitive> flags =
      runtime.makeHandle(std::move(flagsStrRes.getValue()));

  // 8. If flags contains "y", let newFlags be flags.
  // 9. Else, let newFlags be the string that is the concatenation of flags and
  // "y".
  // NOTE: We do not follow this part of the spec, instead, we just use flags as
  // newFlags and actually strip the sticky flag. See below.
  // 10. Let splitter be Construct(C, «rx, newFlags»).
  auto splitterRes = regExpConstructorFastCopy(runtime, regexp, flags);
  if (LLVM_UNLIKELY(splitterRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto splitter = *splitterRes;
  // The spec actually tells us to always set the sticky flag to true, but
  // since it is much faster to perform a global search, we set sticky to
  // false and then check the returned index.
  auto stickyFlags = JSRegExp::getSyntaxFlags(splitter.get());
  auto newFlags = stickyFlags;
  newFlags.sticky = 0;
  JSRegExp::setSyntaxFlags(splitter.get(), newFlags);

  // 6. If flags contains "u", let unicodeMatching be true.
  // 7. Else, let unicodeMatching be false.
  bool unicodeMatching = newFlags.unicode;

  // 11. Let A be ArrayCreate(0).
  auto arrRes = JSArray::create(runtime, 0, 0);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto A = *arrRes;
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
  uint32_t size = S->getStringLength();

  // 15. Let p be 0.
  // Initialise variable indiciating the end of the last match.
  uint32_t p = 0;

  // 16. If lim = 0, return A.
  if (lim == 0) {
    // Don't want any elements, so we're done.
    return A.getHermesValue();
  }

  // 27. If size = 0, then
  if (size == 0) {
    // a. Let z be RegExpExec(splitter, S).
    auto matchResult = JSRegExp::search(splitter, runtime, S, 0);
    if (LLVM_UNLIKELY(matchResult == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    // b. If z is not null, return A.
    else if (!matchResult->empty()) {
      // Matched the entirety of S, so return the empty array.
      return A.getHermesValue();
    }
    // c. Perform CreateDataProperty(A, "0", S).
    // Didn't match S, so add it to the array and return.
    (void)JSArray::setElementAt(A, runtime, 0, S);
    if (LLVM_UNLIKELY(
            JSArray::setLengthProperty(A, runtime, 1) ==
            ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;

    // d. Return A.
    return A.getHermesValue();
  }

  // 18. Let q be p.
  // Place to attempt the start of the next match.
  uint32_t q = p;

  MutableHandle<> tmpHandle{runtime};
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
    auto matchResult = JSRegExp::search(splitter, runtime, S, q);
    if (LLVM_UNLIKELY(matchResult == ExecutionStatus::EXCEPTION))
      return ExecutionStatus::EXCEPTION;

    auto match = *matchResult;

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
      q = advanceStringIndex(S.get(), q, unicodeMatching);
    }
    // iv. Else,
    else {
      // 1. Let T be the String value equal to the substring of S consisting of
      // the code units at indices p (inclusive) through q (exclusive).

      // Found a non-empty string match. Add everything from the last match to
      // the current one to A. This has length q-p because q is the start of the
      // current match, and p was the end (exclusive) of the last match.
      auto strRes = StringPrimitive::slice(runtime, S, p, q - p);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      tmpHandle = *strRes;
      // 2. Perform ! CreateDataPropertyOrThrow(A, ! ToString(lengthA), T).
      JSArray::setElementAt(A, runtime, lengthA, tmpHandle);
      // 3. Set lengthA to lengthA + 1.
      ++lengthA;

      // 4. If lengthA = lim, return A.
      if (lengthA == lim) {
        if (LLVM_UNLIKELY(
                JSArray::setLengthProperty(A, runtime, lengthA) ==
                ExecutionStatus::EXCEPTION))
          return ExecutionStatus::EXCEPTION;
        return A.getHermesValue();
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
          JSArray::setElementAt(
              A, runtime, lengthA, Runtime::getUndefinedValue());
        } else {
          if (LLVM_UNLIKELY(
                  (strRes = StringPrimitive::slice(
                       runtime, S, range->location, range->length)) ==
                  ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          JSArray::setElementAt(
              A,
              runtime,
              lengthA,
              runtime.makeHandle<StringPrimitive>(*strRes));
        }
        // d. Let lengthA be lengthA +1.
        ++lengthA;
        // e. If lengthA = lim, return A.
        if (lengthA == lim) {
          if (LLVM_UNLIKELY(
                  JSArray::setLengthProperty(A, runtime, lengthA) ==
                  ExecutionStatus::EXCEPTION))
            return ExecutionStatus::EXCEPTION;
          return A.getHermesValue();
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
  auto elementStrRes = StringPrimitive::slice(runtime, S, p, size - p);
  if (LLVM_UNLIKELY(elementStrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  tmpHandle = *elementStrRes;
  // 21. Perform ! CreateDataPropertyOrThrow(A, ! ToString(lengthA), T).
  JSArray::setElementAt(A, runtime, lengthA, tmpHandle);
  ++lengthA;

  if (LLVM_UNLIKELY(
          JSArray::setLengthProperty(A, runtime, lengthA) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  // 22. Return A.
  return A.getHermesValue();
}

// ES2022 22.2.5.4
// Note that we don't yet support unicode.
CallResult<HermesValue>
regExpFlagsGetter(void *ctx, Runtime &runtime, NativeArgs args) {
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

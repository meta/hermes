/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JSRegExpStringIterator.h"

#include "hermes/VM/BuildMetadata.h"
#include "hermes/VM/StringPrimitive.h"

#include "JSLib/JSLibInternal.h"

namespace hermes {
namespace vm {

//===----------------------------------------------------------------------===//
// class JSRegExpStringIterator

const ObjectVTable JSRegExpStringIterator::vt{
    VTable(
        CellKind::JSRegExpStringIteratorKind,
        cellSize<JSRegExpStringIterator>()),
    JSRegExpStringIterator::_getOwnIndexedRangeImpl,
    JSRegExpStringIterator::_haveOwnIndexedImpl,
    JSRegExpStringIterator::_getOwnIndexedPropertyFlagsImpl,
    JSRegExpStringIterator::_getOwnIndexedImpl,
    JSRegExpStringIterator::_setOwnIndexedImpl,
    JSRegExpStringIterator::_deleteOwnIndexedImpl,
    JSRegExpStringIterator::_checkAllOwnIndexedImpl,
};

void JSRegExpStringIteratorBuildMeta(
    const GCCell *cell,
    Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(
      JSObject::numOverlapSlots<JSRegExpStringIterator>());
  JSObjectBuildMeta(cell, mb);
  const auto *self = static_cast<const JSRegExpStringIterator *>(cell);
  mb.setVTable(&JSRegExpStringIterator::vt);
  mb.addField("iteratedRegExp", &self->iteratedRegExp_);
  mb.addField("iteratedString", &self->iteratedString_);
}

/// ES11 21.2.5.8.1 CreateRegExpStringIterator ( R, S, global, fullUnicode )
PseudoHandle<JSRegExpStringIterator> JSRegExpStringIterator::create(
    Runtime &runtime,
    Handle<JSObject> R,
    Handle<StringPrimitive> S,
    bool global,
    bool fullUnicode) {
  auto proto = Handle<JSObject>::vmcast(&runtime.regExpStringIteratorPrototype);

  auto *cell = runtime.makeAFixed<JSRegExpStringIterator>(
      runtime,
      proto,
      runtime.getHiddenClassForPrototype(
          *proto, runtime.classJSRegExpStringIterator),
      R,
      S,
      global,
      fullUnicode);
  return JSObjectInit::initToPseudoHandle(runtime, cell);
}

/// ES11 21.2.7.1.1 %RegExpStringIteratorPrototype%.next ( ) 4-11
CallResult<HermesValue> JSRegExpStringIterator::nextElement(
    Handle<JSRegExpStringIterator> O,
    Runtime &runtime) {
  struct : public Locals {
    PinnedValue<JSObject> R;
    PinnedValue<StringPrimitive> S;
    PinnedValue<JSObject> matchObj;
    PinnedValue<> prop;
    PinnedValue<StringPrimitive> matchStr;
    PinnedValue<> lastIndex;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  // 4. If O.[[Done]] is true, then
  if (O->done_) {
    // a. Return ! CreateIterResultObject(undefined, true).
    return createIterResultObject(runtime, Runtime::getUndefinedValue(), true);
  }
  // 5. Let R be O.[[IteratingRegExp]].
  lv.R = O->iteratedRegExp_.getNonNull(runtime);
  // 6. Let S be O.[[IteratedString]].
  lv.S = O->iteratedString_.getNonNull(runtime);

  // 7. Let global be O.[[Global]].
  // 8. Let fullUnicode be O.[[Unicode]].
  // we will just use JSRegExpStringIterator's fields for non pointer types.

  // 9. Let match be ? RegExpExec(R, S).
  auto matchRes = regExpExec(runtime, lv.R, lv.S);
  if (LLVM_UNLIKELY(matchRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto match = matchRes.getValue();

  // 10. If match is null, then
  if (match.isNull()) {
    // a. Set O.[[Done]] to true.
    O->done_ = true;
    // b. Return ! CreateIterResultObject(undefined, true).
    return createIterResultObject(runtime, Runtime::getUndefinedValue(), true);
  } else {
    // 11. Else,
    lv.matchObj.castAndSetHermesValue<JSObject>(match);
    // a. If global is true, then
    if (O->global_) {
      //  i. Let matchStr be ? ToString(? Get(match, "0")).
      auto propRes = getIndexed_RJS(runtime, lv.matchObj, 0);
      if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.prop = std::move(*propRes);
      auto matchStrRes = toString_RJS(runtime, lv.prop);
      if (matchStrRes == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.matchStr = std::move(*matchStrRes);

      //  ii. If matchStr is the empty String, then
      if (lv.matchStr->getStringLength() == 0) {
        // 1. Let thisIndex be ? ToLength(? Get(R, "lastIndex")).
        auto lastIndexRes =
            runtime.getNamed(lv.R, PropCacheID::RegExpLastIndex);
        if (lastIndexRes == ExecutionStatus::EXCEPTION) {
          return ExecutionStatus::EXCEPTION;
        }
        lv.lastIndex = std::move(*lastIndexRes);
        auto thisIndex = toLength(runtime, lv.lastIndex);
        if (thisIndex == ExecutionStatus::EXCEPTION) {
          return ExecutionStatus::EXCEPTION;
        }
        // 2. Let nextIndex be ! AdvanceStringIndex(S, thisIndex, fullUnicode).
        double nextIndex = advanceStringIndex(
            lv.S.get(), thisIndex->getNumberAs<uint64_t>(), O->unicode_);
        // 3. Perform ? Set(R, "lastIndex", nextIndex, true).
        if (setLastIndex(lv.R, runtime, nextIndex) ==
            ExecutionStatus::EXCEPTION) {
          return ExecutionStatus::EXCEPTION;
        }
      }
      // iii. Return ! CreateIterResultObject(match, false).
      return createIterResultObject(runtime, lv.matchObj, false);
    }
    // b. Else,
    else {
      // i. Set O.[[Done]] to true.
      O->done_ = true;
      // ii. Return ! CreateIterResultObject(match, false).
      return createIterResultObject(runtime, lv.matchObj, false);
    }
  }
}

} // namespace vm
} // namespace hermes

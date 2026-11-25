/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES11 21.2.7 RegExp String Iterator Objects
//===----------------------------------------------------------------------===//
#include "JSLibInternal.h"

#include "hermes/VM/JSRegExpStringIterator.h"
#include "hermes/VM/Runtime.h"

namespace hermes {
namespace vm {

void populateRegExpStringIteratorPrototype(Runtime &runtime) {
  struct : public Locals {
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto proto = Handle<JSObject>::vmcast(&runtime.regExpStringIteratorPrototype);

  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::next),
      nullptr,
      regExpStringIteratorPrototypeNext,
      0);

  // ES11 21.2.7.1.2 %RegExpStringIteratorPrototype% [ @@toStringTag ]
  auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  lv.str = runtime.getPredefinedString(Predefined::RegExpStringIterator);
  defineProperty(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      lv.str,
      dpf);
}

/// ES11 21.2.7.1.1 %RegExpStringIteratorPrototype%.next ( ) 1-3
CallResult<HermesValue> regExpStringIteratorPrototypeNext(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. Let O be the this value.
  // 2. If Type(O) is not Object, throw a TypeError exception.
  // 3. If O does not have all of the internal slots of a RegExp String Iterator
  // Object Instance (see 21.2.7.2), throw a TypeError exception.
  auto O = args.dyncastThis<JSRegExpStringIterator>();
  if (LLVM_UNLIKELY(!O)) {
    return runtime.raiseTypeError(
        "RegExpStringIteratorPrototype.next requires 'this' is a RegExp String Iterator");
  }
  return JSRegExpStringIterator::nextElement(O, runtime);
}

} // namespace vm
} // namespace hermes

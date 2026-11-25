/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES6.0 21.1.5 String Iterator Objects
//===----------------------------------------------------------------------===//
#include "JSLibInternal.h"

#include "hermes/VM/PrimitiveBox.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StringPrimitive.h"

namespace hermes {
namespace vm {

void populateStringIteratorPrototype(Runtime &runtime) {
  struct : public Locals {
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto proto = Handle<JSObject>::vmcast(&runtime.stringIteratorPrototype);

  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::next),
      nullptr,
      stringIteratorPrototypeNext,
      0);

  auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  lv.str = runtime.getPredefinedString(Predefined::StringIterator);
  defineProperty(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      lv.str,
      dpf);
}

/// ES6.0 21.1.5.2.1 %StringIteratorPrototype%.next ( ) 1-3
CallResult<HermesValue> stringIteratorPrototypeNext(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. Let O be the this value.
  // 2. If Type(O) is not Object, throw a TypeError exception.
  // 3. If O does not have all of the internal slots of a String Iterator
  // Instance (21.1.5.3), throw a TypeError exception.
  auto O = args.dyncastThis<JSStringIterator>();
  if (LLVM_UNLIKELY(!O)) {
    return runtime.raiseTypeError(
        "StringIteratorPrototype.next requires 'this' is a String Iterator");
  }
  return JSStringIterator::nextElement(O, runtime);
}

} // namespace vm
} // namespace hermes

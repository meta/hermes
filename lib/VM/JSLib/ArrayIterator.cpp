/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES6.0 22.1.5 Array Iterator Objects
//===----------------------------------------------------------------------===//
#include "JSLibInternal.h"

#include "hermes/VM/JSArray.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StringPrimitive.h"

namespace hermes {
namespace vm {

void populateArrayIteratorPrototype(Runtime &runtime) {
  struct : public Locals {
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto proto = Handle<JSObject>::vmcast(&runtime.arrayIteratorPrototype);

  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::next),
      nullptr,
      arrayIteratorPrototypeNext,
      0);

  auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  lv.str = runtime.getPredefinedString(Predefined::ArrayIterator);
  defineProperty(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      lv.str,
      dpf);
}

CallResult<HermesValue> arrayIteratorPrototypeNext(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto O = args.dyncastThis<JSArrayIterator>();
  if (LLVM_UNLIKELY(!O)) {
    return runtime.raiseTypeError(
        "ArrayIteratorPrototype.next requires that 'this' be an Array Iterator");
  }
  return JSArrayIterator::nextElement(O, runtime);
}

} // namespace vm
} // namespace hermes

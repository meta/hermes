/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES6.0 25.1.2 The %IteratorPrototype% Object
//===----------------------------------------------------------------------===//
#include "JSLibInternal.h"

#include "hermes/VM/Runtime.h"

namespace hermes {
namespace vm {

void populateIteratorPrototype(Runtime &runtime) {
  struct : public Locals {
    PinnedValue<NativeFunction> iteratorFunc;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto proto = Handle<JSObject>::vmcast(&runtime.iteratorPrototype);

  lv.iteratorFunc = NativeFunction::create(
      runtime,
      nullptr,
      iteratorPrototypeIterator,
      Predefined::getSymbolID(Predefined::squareSymbolIterator),
      0);

  defineProperty(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::SymbolIterator),
      lv.iteratorFunc);
}

CallResult<HermesValue> iteratorPrototypeIterator(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return args.getThisArg();
}

} // namespace vm
} // namespace hermes

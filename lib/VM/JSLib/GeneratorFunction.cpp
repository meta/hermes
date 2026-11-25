/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES6.0 25.2.1 The GeneratorFunction constructor.
//===----------------------------------------------------------------------===//
#include "JSLibInternal.h"

namespace hermes {
namespace vm {

HermesValue createGeneratorFunctionConstructor(Runtime &runtime) {
  auto proto = Handle<JSObject>::vmcast(&runtime.generatorFunctionPrototype);

  struct : public Locals {
    PinnedValue<NativeConstructor> cons;
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto consRes = NativeConstructor::create(
      runtime,
      Handle<JSObject>::vmcast(&runtime.functionConstructor),
      nullptr,
      generatorFunctionConstructor,
      1);
  lv.cons.castAndSetHermesValue<NativeConstructor>(consRes.getHermesValue());

  auto st = Callable::defineNameLengthAndPrototype(
      lv.cons,
      runtime,
      Predefined::getSymbolID(Predefined::GeneratorFunction),
      1,
      proto,
      Callable::WritablePrototype::No);
  (void)st;
  assert(
      st != ExecutionStatus::EXCEPTION && "defineLengthAndPrototype() failed");

  auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;

  // The initial value of GeneratorFunction.prototype.constructor is the
  // intrinsic object %GeneratorFunction%.
  defineProperty(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::constructor),
      lv.cons,
      dpf);

  // The value of GeneratorFunction.prototype.prototype is the
  // %GeneratorPrototype% intrinsic object.
  defineProperty(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::prototype),
      runtime.generatorPrototype,
      dpf);

  lv.str = runtime.getPredefinedString(Predefined::GeneratorFunction);
  defineProperty(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      lv.str,
      dpf);

  return lv.cons.getHermesValue();
}

CallResult<HermesValue> generatorFunctionConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return createDynamicFunction(runtime, args, DynamicFunctionKind::Generator);
}

} // namespace vm
} // namespace hermes

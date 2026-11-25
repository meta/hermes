/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES6.0 25.3.1 The %GeneratorPrototype% Object
//===----------------------------------------------------------------------===//
#include "JSLibInternal.h"

#include "hermes/BCGen/GeneratorResumeMethod.h"
#include "hermes/VM/JSGeneratorObject.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/Runtime.h"

namespace hermes {
namespace vm {

void populateGeneratorPrototype(Runtime &runtime) {
  struct : public Locals {
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto proto = Handle<JSObject>::vmcast(&runtime.generatorPrototype);

  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::next),
      (void *)Action::Next,
      generatorPrototypeResume,
      1);

  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::returnStr),
      (void *)Action::Return,
      generatorPrototypeResume,
      1);
  defineMethod(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::throwStr),
      (void *)Action::Throw,
      generatorPrototypeResume,
      1);

  auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;

  // The initial value of GeneratorFunction.prototype.constructor is the
  // intrinsic object %Generator% (which is GeneratorFunction.prototype).
  defineProperty(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::constructor),
      runtime.generatorFunctionPrototype,
      dpf);

  lv.str = runtime.getPredefinedString(Predefined::Generator);
  defineProperty(
      runtime,
      proto,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      lv.str,
      dpf);
}

// ES6.0 25.3.3.2.
static CallResult<Handle<JSGeneratorObject>> generatorValidate(
    Runtime &runtime,
    Handle<> value) {
  auto generator = Handle<JSGeneratorObject>::dyn_vmcast(value);
  if (!generator) {
    return runtime.raiseTypeError(
        "Generator functions must be called on generators");
  }
  return generator;
}

// The logic for ES6.0 25.3.1.(2|3|4) is handled by the inner
// function. All we have to do here is give it the proper action and value.
CallResult<HermesValue> generatorPrototypeResume(void *ctx, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto generatorRes = generatorValidate(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(generatorRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  struct : public Locals {
    PinnedValue<Callable> innerFunc;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  Action action = *reinterpret_cast<Action *>(&ctx);
  lv.innerFunc =
      JSGeneratorObject::getInnerFunction(runtime, generatorRes->get());
  auto value = args.getArgHandle(0);
  auto valueRes = Callable::executeCall2(
      lv.innerFunc,
      runtime,
      lv.innerFunc,
      HermesValue::encodeTrustedNumberValue((uint8_t)action),
      value.getHermesValue());
  if (LLVM_UNLIKELY(valueRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  assert(
      (*valueRes)->isObject() &&
      "inner generator function must return an object");
  return valueRes->getHermesValue();
}

} // namespace vm
} // namespace hermes

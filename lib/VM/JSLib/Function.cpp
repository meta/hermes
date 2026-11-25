/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES5.1 15.3 Initialize the Function constructor.
//===----------------------------------------------------------------------===//
#include "JSLibInternal.h"

#include "hermes/Regex/Executor.h"
#include "hermes/Regex/RegexTraits.h"
#include "hermes/VM/ArrayLike.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/PropertyAccessor.h"
#include "hermes/VM/Runtime-inline.h"
#include "hermes/VM/RuntimeModule.h"
#include "hermes/VM/StringBuilder.h"
#include "hermes/VM/StringView.h"

namespace hermes {
namespace vm {

//===----------------------------------------------------------------------===//
/// Function.

HermesValue createFunctionConstructor(Runtime &runtime) {
  auto functionPrototype = Handle<Callable>::vmcast(&runtime.functionPrototype);

  struct : public Locals {
    PinnedValue<NativeConstructor> cons;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  defineSystemConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::Function),
      functionConstructor,
      functionPrototype,
      1,
      lv.cons);

  // Function.prototype.xxx() methods.
  defineMethod(
      runtime,
      functionPrototype,
      Predefined::getSymbolID(Predefined::toString),
      nullptr,
      functionPrototypeToString,
      0);
  runtime.registerBuiltin(
      BuiltinMethod::HermesBuiltin_functionPrototypeApply,
      defineMethod(
          runtime,
          functionPrototype,
          Predefined::getSymbolID(Predefined::apply),
          nullptr,
          functionPrototypeApply,
          2));

  runtime.registerBuiltin(
      BuiltinMethod::HermesBuiltin_functionPrototypeCall,
      defineMethod(
          runtime,
          functionPrototype,
          Predefined::getSymbolID(Predefined::call),
          nullptr,
          functionPrototypeCall,
          1));
  defineMethod(
      runtime,
      functionPrototype,
      Predefined::getSymbolID(Predefined::bind),
      nullptr,
      functionPrototypeBind,
      1);

  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  dpf.configurable = 0;
  runtime.functionPrototypeSymbolHasInstance = defineMethod(
      runtime,
      functionPrototype,
      Predefined::getSymbolID(Predefined::SymbolHasInstance),
      Predefined::getSymbolID(Predefined::squareSymbolHasInstance),
      nullptr,
      functionPrototypeSymbolHasInstance,
      1,
      dpf);

  // Define .callee and .arguments properties to throw always. In accordance
  // with ES2023 10.2.4 AddRestrictedFunctionProperties.
  auto accessor =
      Handle<PropertyAccessor>::vmcast(&runtime.throwTypeErrorAccessor);
  PropertyFlags pf;
  pf.clear();
  pf.enumerable = 0;
  pf.configurable = 1;
  pf.accessor = 1;
  auto res = JSObject::defineNewOwnProperty(
      functionPrototype,
      runtime,
      Predefined::getSymbolID(Predefined::caller),
      pf,
      accessor);
  (void)res;
  assert(res != ExecutionStatus::EXCEPTION && "defineNewOwnProperty() failed");
  res = JSObject::defineNewOwnProperty(
      functionPrototype,
      runtime,
      Predefined::getSymbolID(Predefined::arguments),
      pf,
      accessor);
  (void)res;
  assert(res != ExecutionStatus::EXCEPTION && "defineNewOwnProperty() failed");

  return lv.cons.getHermesValue();
}

CallResult<HermesValue> functionConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return createDynamicFunction(runtime, args, DynamicFunctionKind::Normal);
}

CallResult<HermesValue> functionPrototypeToString(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  struct : public Locals {
    PinnedValue<> propValue;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto func = args.dyncastThis<Callable>();
  if (!func) {
    return runtime.raiseTypeError(
        "Can't call Function.prototype.toString() on non-callable");
  }

  /// Append the current function name to the \p strBuf.
  auto appendFunctionName = [&func, &runtime, &lv](SmallU16String<64> &strBuf) {
    // Extract the name.
    auto propRes = JSObject::getNamed_RJS(
        func, runtime, Predefined::getSymbolID(Predefined::name));
    if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // Convert the name to string, unless it is undefined.
    if (!(*propRes)->isUndefined()) {
      lv.propValue = std::move(*propRes);
      auto strRes = toString_RJS(runtime, lv.propValue);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      strRes->get()->appendUTF16String(strBuf);
    }
    return ExecutionStatus::RETURNED;
  };

  // Deal with JSFunctions that has a source String ID. That implies this
  // function need a non-default toString implementation.
  if (auto jsFunc = dyn_vmcast<JSFunction>(*func)) {
    RuntimeModule *runtimeModule = jsFunc->getCodeBlock()->getRuntimeModule();
    bool nativeCode = false;

    // If the module's functions are builtins, all show [native code].
    if (runtimeModule->funcsAreBuiltins()) {
      nativeCode = true;
    } else if (auto sourceID = jsFunc->getCodeBlock()->getFunctionSourceID()) {
      StringPrimitive *source =
          runtimeModule->getStringPrimFromStringIDMayAllocate(*sourceID);
      // Empty source marks implementation-hidden function, fabricate a source
      // code string that imitate a NativeFunction.
      if (source->getStringLength() == 0) {
        nativeCode = true;
      } else {
        // Otherwise, it's the preserved source code.
        return HermesValue::encodeStringValue(source);
      }
    }

    if (nativeCode) {
      SmallU16String<64> strBuf{};
      strBuf.append("function ");
      if (LLVM_UNLIKELY(
              appendFunctionName(strBuf) == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      strBuf.append("() { [native code] }");
      return StringPrimitive::create(runtime, strBuf);
    }
  }

  SmallU16String<64> strBuf{};
  if (Callable::isAsyncFunction(*func)) {
    strBuf.append("async function ");
  } else if (Callable::isGeneratorFunction(*func)) {
    strBuf.append("function *");
  } else {
    strBuf.append("function ");
  }

  if (LLVM_UNLIKELY(appendFunctionName(strBuf) == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // Formal parameters and the rest of the body.
  if (vmisa<NativeFunction>(*func)) {
    // Use [native code] here because we want to work with tools like Babel
    // which detect the string "[native code]" and use it to alter behavior
    // during the class transform.
    // Also print without synthesized formal parameters to avoid breaking
    // heuristics that detect the string "() { [native code] }".
    // \see https://github.com/facebook/hermes/issues/471
    strBuf.append("() { [native code] }");
  } else {
    // Append the synthesized formal parameters.
    strBuf.append('(');

    // Extract ".length".
    auto lengthProp = Callable::extractOwnLengthProperty_RJS(func, runtime);
    if (lengthProp == ExecutionStatus::EXCEPTION)
      return ExecutionStatus::EXCEPTION;

    // The value of the property is not guaranteed to be meaningful, so clamp it
    // to [0..65535] for sanity.
    uint32_t paramCount =
        (uint32_t)std::min(65535.0, std::max(0.0, *lengthProp));

    for (uint32_t i = 0; i < paramCount; ++i) {
      if (i != 0)
        strBuf.append(", ");
      char buf[16];
      ::snprintf(buf, sizeof(buf), "a%u", i);
      strBuf.append(buf);
    }

    // Avoid using the [native code] string to prevent extra wrapping overhead
    // in, e.g., Babel's class extension mechanism.
    strBuf.append(") { [bytecode] }");
  }

  // Finally allocate a StringPrimitive.
  return StringPrimitive::create(runtime, strBuf);
} // namespace vm

CallResult<HermesValue> functionPrototypeApply(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto func = args.dyncastThis<Callable>();
  if (LLVM_UNLIKELY(!func)) {
    return runtime.raiseTypeError("Can't apply() to non-callable");
  }

  if (args.getArg(1).isNull() || args.getArg(1).isUndefined()) {
    ScopedNativeCallFrame newFrame{runtime, 0, *func, false, args.getArg(0)};
    if (LLVM_UNLIKELY(newFrame.overflowed()))
      return runtime.raiseStackOverflow(
          Runtime::StackOverflowKind::NativeStack);
    return Callable::call(func, runtime).toCallResultHermesValue();
  }

  auto argObj = Handle<JSObject>::dyn_vmcast(args.getArgHandle(1));
  if (LLVM_UNLIKELY(!argObj)) {
    return runtime.raiseTypeError(
        "Can't apply() with non-object arguments list");
  }

  return Callable::executeCall(
             func,
             runtime,
             Runtime::getUndefinedValue(),
             args.getArgHandle(0),
             argObj)
      .toCallResultHermesValue();
}

CallResult<HermesValue> functionPrototypeCall(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto func = args.dyncastThis<Callable>();
  if (LLVM_UNLIKELY(!func)) {
    return runtime.raiseTypeError("Can't call() non-callable");
  }

  uint32_t argCount = args.getArgCount();
  ScopedNativeCallFrame newFrame{
      runtime, argCount ? argCount - 1 : 0, *func, false, args.getArg(0)};
  if (LLVM_UNLIKELY(newFrame.overflowed()))
    return runtime.raiseStackOverflow(Runtime::StackOverflowKind::NativeStack);
  for (uint32_t i = 1; i < argCount; ++i) {
    newFrame->getArgRef(i - 1) = args.getArg(i);
  }
  auto res = Callable::call(func, runtime);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return res->getHermesValue();
}

CallResult<HermesValue> functionPrototypeBind(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto target = Handle<Callable>::dyn_vmcast(args.getThisHandle());
  if (!target) {
    return runtime.raiseTypeError("Can't bind() a non-callable");
  }

  return BoundFunction::create(
      runtime, target, args.getArgCount(), args.begin());
}

CallResult<HermesValue> functionPrototypeSymbolHasInstance(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  /// 1. Let F be the this value.
  auto F = args.getThisHandle();
  /// 2. Return OrdinaryHasInstance(F, V).
  auto result = ordinaryHasInstance(runtime, F, args.getArgHandle(0));
  if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return HermesValue::encodeBoolValue(*result);
}

} // namespace vm
} // namespace hermes

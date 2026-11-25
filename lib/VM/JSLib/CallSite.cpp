/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// Initialize the internal CallSite prototype.
//===----------------------------------------------------------------------===//

#include "JSLibInternal.h"
#include "hermes/VM/JSCallSite.h"
#include "hermes/VM/StringPrimitive.h"

namespace hermes {
namespace vm {

void populateCallSitePrototype(Runtime &runtime) {
  struct : public Locals {
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  auto callSitePrototype = Handle<JSObject>::vmcast(&runtime.callSitePrototype);

  // CallSite.prototype.xxx methods.
  defineMethod(
      runtime,
      callSitePrototype,
      Predefined::getSymbolID(Predefined::getFunctionName),
      nullptr,
      callSitePrototypeGetFunctionName,
      0);
  defineMethod(
      runtime,
      callSitePrototype,
      Predefined::getSymbolID(Predefined::getFileName),
      nullptr,
      callSitePrototypeGetFileName,
      0);
  defineMethod(
      runtime,
      callSitePrototype,
      Predefined::getSymbolID(Predefined::getLineNumber),
      nullptr,
      callSitePrototypeGetLineNumber,
      0);
  defineMethod(
      runtime,
      callSitePrototype,
      Predefined::getSymbolID(Predefined::getColumnNumber),
      nullptr,
      callSitePrototypeGetColumnNumber,
      0);
  defineMethod(
      runtime,
      callSitePrototype,
      Predefined::getSymbolID(Predefined::getBytecodeAddress),
      nullptr,
      callSitePrototypeGetBytecodeAddress,
      0);
  defineMethod(
      runtime,
      callSitePrototype,
      Predefined::getSymbolID(Predefined::isNative),
      nullptr,
      callSitePrototypeIsNative,
      0);
  defineMethod(
      runtime,
      callSitePrototype,
      Predefined::getSymbolID(Predefined::getThis),
      nullptr,
      callSitePrototypeGetThis,
      0);
  defineMethod(
      runtime,
      callSitePrototype,
      Predefined::getSymbolID(Predefined::getTypeName),
      nullptr,
      callSitePrototypeGetTypeName,
      0);
  defineMethod(
      runtime,
      callSitePrototype,
      Predefined::getSymbolID(Predefined::getFunction),
      nullptr,
      callSitePrototypeGetFunction,
      0);
  defineMethod(
      runtime,
      callSitePrototype,
      Predefined::getSymbolID(Predefined::getMethodName),
      nullptr,
      callSitePrototypeGetMethodName,
      0);
  defineMethod(
      runtime,
      callSitePrototype,
      Predefined::getSymbolID(Predefined::getEvalOrigin),
      nullptr,
      callSitePrototypeGetEvalOrigin,
      0);
  defineMethod(
      runtime,
      callSitePrototype,
      Predefined::getSymbolID(Predefined::isToplevel),
      nullptr,
      callSitePrototypeIsToplevel,
      0);
  defineMethod(
      runtime,
      callSitePrototype,
      Predefined::getSymbolID(Predefined::isEval),
      nullptr,
      callSitePrototypeIsEval,
      0);
  defineMethod(
      runtime,
      callSitePrototype,
      Predefined::getSymbolID(Predefined::isConstructor),
      nullptr,
      callSitePrototypeIsConstructor,
      0);
  defineMethod(
      runtime,
      callSitePrototype,
      Predefined::getSymbolID(Predefined::isAsync),
      nullptr,
      callSitePrototypeIsAsync,
      0);
  defineMethod(
      runtime,
      callSitePrototype,
      Predefined::getSymbolID(Predefined::isPromiseAll),
      nullptr,
      callSitePrototypeIsPromiseAll,
      0);
  defineMethod(
      runtime,
      callSitePrototype,
      Predefined::getSymbolID(Predefined::getPromiseIndex),
      nullptr,
      callSitePrototypeGetPromiseIndex,
      0);

  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  lv.str = runtime.getPredefinedString(Predefined::CallSite);
  defineProperty(
      runtime,
      callSitePrototype,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      lv.str,
      dpf);
}

namespace {
template <auto Fn>
static CallResult<HermesValue> ensureJSCallSiteAndCall(
    Handle<> self,
    Runtime &runtime) {
  if (auto jsCallSiteSelf = Handle<JSCallSite>::dyn_vmcast(self)) {
    return (*Fn)(runtime, jsCallSiteSelf);
  }

  return runtime.raiseTypeError(
      "CallSite method called on an incompatible receiver");
}
} // namespace

CallResult<HermesValue> callSitePrototypeGetFunctionName(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return ensureJSCallSiteAndCall<&JSCallSite::getFunctionName>(
      args.getThisHandle(), runtime);
}

CallResult<HermesValue> callSitePrototypeGetFileName(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return ensureJSCallSiteAndCall<&JSCallSite::getFileName>(
      args.getThisHandle(), runtime);
}

CallResult<HermesValue> callSitePrototypeGetLineNumber(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return ensureJSCallSiteAndCall<&JSCallSite::getLineNumber>(
      args.getThisHandle(), runtime);
}

CallResult<HermesValue> callSitePrototypeGetColumnNumber(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return ensureJSCallSiteAndCall<&JSCallSite::getColumnNumber>(
      args.getThisHandle(), runtime);
}

CallResult<HermesValue> callSitePrototypeGetBytecodeAddress(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return ensureJSCallSiteAndCall<&JSCallSite::getBytecodeAddress>(
      args.getThisHandle(), runtime);
}

CallResult<HermesValue> callSitePrototypeIsNative(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return ensureJSCallSiteAndCall<&JSCallSite::isNative>(
      args.getThisHandle(), runtime);
}

CallResult<HermesValue> callSitePrototypeGetThis(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return ensureJSCallSiteAndCall<&JSCallSite::getThis>(
      args.getThisHandle(), runtime);
}

CallResult<HermesValue> callSitePrototypeGetTypeName(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return ensureJSCallSiteAndCall<&JSCallSite::getTypeName>(
      args.getThisHandle(), runtime);
}

CallResult<HermesValue> callSitePrototypeGetFunction(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return ensureJSCallSiteAndCall<&JSCallSite::getFunction>(
      args.getThisHandle(), runtime);
}

CallResult<HermesValue> callSitePrototypeGetMethodName(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return ensureJSCallSiteAndCall<&JSCallSite::getMethodName>(
      args.getThisHandle(), runtime);
}

CallResult<HermesValue> callSitePrototypeGetEvalOrigin(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return ensureJSCallSiteAndCall<&JSCallSite::getEvalOrigin>(
      args.getThisHandle(), runtime);
}

CallResult<HermesValue> callSitePrototypeIsToplevel(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return ensureJSCallSiteAndCall<&JSCallSite::isToplevel>(
      args.getThisHandle(), runtime);
}

CallResult<HermesValue> callSitePrototypeIsEval(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return ensureJSCallSiteAndCall<&JSCallSite::isEval>(
      args.getThisHandle(), runtime);
}

CallResult<HermesValue> callSitePrototypeIsConstructor(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return ensureJSCallSiteAndCall<&JSCallSite::isConstructor>(
      args.getThisHandle(), runtime);
}

CallResult<HermesValue> callSitePrototypeIsAsync(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return ensureJSCallSiteAndCall<&JSCallSite::isAsync>(
      args.getThisHandle(), runtime);
}

CallResult<HermesValue> callSitePrototypeIsPromiseAll(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return ensureJSCallSiteAndCall<&JSCallSite::isPromiseAll>(
      args.getThisHandle(), runtime);
}

CallResult<HermesValue> callSitePrototypeGetPromiseIndex(
    void *,
    Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return ensureJSCallSiteAndCall<&JSCallSite::getPromiseIndex>(
      args.getThisHandle(), runtime);
}

} // namespace vm
} // namespace hermes

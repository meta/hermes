/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES9 26.2 Proxy Objects
//===----------------------------------------------------------------------===//

#include "JSLibInternal.h"

#include "hermes/VM/JSCallableProxy.h"
#include "hermes/VM/StackFrame-inline.h"

namespace hermes {
namespace vm {

namespace {

// Property storage slots.  This implementation for storing internal
// properties is not a great pattern, and may change.
enum ProxySlotIndexes { revocableProxy, COUNT };

SmallHermesValue getRevocableProxySlot(
    NativeFunction *revoker,
    Runtime &runtime) {
  return NativeFunction::getAdditionalSlotValue(
      revoker, runtime, ProxySlotIndexes::revocableProxy);
}

void setRevocableProxySlot(
    NativeFunction *revoker,
    Runtime &runtime,
    SmallHermesValue value) {
  NativeFunction::setAdditionalSlotValue(
      revoker, runtime, ProxySlotIndexes::revocableProxy, value);
}

// This is shared code between the proxy constructor and the native
// Proxy.revocable factory.
// \param proxy is the newly created but not yet initialized object
// used as the constructor's this.
// \param resultProxy MutableHandle to store the final proxy result
ExecutionStatus proxyCreate(
    Runtime &runtime,
    Handle<JSObject> target,
    Handle<JSObject> handler,
    Handle<JSObject> proxy,
    MutableHandle<JSObject> resultProxy) {
  // 1. If Type(target) is not Object, throw a TypeError exception.
  if (!target) {
    return runtime.raiseTypeError("new Proxy target must be an Object");
  }
  // 3. If Type(handler) is not Object, throw a TypeError exception.
  if (!handler) {
    return runtime.raiseTypeError("new Proxy handler must be an Object");
  }
  // 5. Let P be a newly created object.
  // 6. Set P’s essential internal methods (except for [[Call]] and
  // [[Construct]]) to the definitions specified in 9.5.
  struct : public Locals {
    PinnedValue<JSCallableProxy> callableProxy;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 7. If IsCallable(target) is true, then
  if (vmisa<Callable>(*target)) {
    //   a. Set P.[[Call]] as specified in 9.5.12.
    //   b. If IsConstructor(target) is true, then
    //     i. Set P.[[Construct]] as specified in 9.5.13.
    // We need to throw away the object passed as this, so we can create
    // a CallableProxy instead.  Simply being a CallableProxy has the
    // effect of setting the [[Call]] and [[Construct]] internal methods.
    lv.callableProxy = JSCallableProxy::create(runtime);
    proxy = lv.callableProxy;
  }

  // 8. Set the [[ProxyTarget]] internal slot of P to target.
  // 9. Set the [[ProxyHandler]] internal slot of P to handler.

  JSProxy::setTargetAndHandler(proxy, runtime, target, handler);

  // Store the result in the output parameter.
  resultProxy.castAndSetHermesValue<JSObject>(proxy.getHermesValue());
  return ExecutionStatus::RETURNED;
}

} // namespace

CallResult<HermesValue> proxyConstructor(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  // 1. If NewTarget is undefined, throw a TypeError exception.
  if (!args.isConstructorCall()) {
    return runtime.raiseTypeError(
        "Proxy() called in function context instead of constructor");
  }
  // 2. Return ? ProxyCreate(target, handler).
  struct : public Locals {
    PinnedValue<JSProxy> self;
    PinnedValue<JSObject> result;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  lv.self = JSProxy::create(runtime);
  auto status = proxyCreate(
      runtime,
      args.dyncastArg<JSObject>(0),
      args.dyncastArg<JSObject>(1),
      lv.self,
      MutableHandle{lv.result});
  if (status == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return lv.result.getHermesValue();
}

CallResult<HermesValue> proxyRevocationSteps(void *, Runtime &runtime) {
  // 1. Let p be F.[[RevocableProxy]].
  auto revoker = vmcast<NativeFunction>(
      runtime.getCurrentFrame()->getCalleeClosureUnsafe());
  SmallHermesValue proxyVal = getRevocableProxySlot(revoker, runtime);
  // 2. If p is null, return undefined.
  if (proxyVal.isNull()) {
    return HermesValue::encodeUndefinedValue();
  }
  // 3. Set F.[[RevocableProxy]] to null.
  setRevocableProxySlot(revoker, runtime, SmallHermesValue::encodeNullValue());
  // 4. Assert: p is a Proxy object.
  JSObject *proxy = vmcast<JSObject>(proxyVal.getObject(runtime));
  assert(proxy->isProxyObject(runtime) && "[[RevocableProxy]] is not a Proxy");

  struct : public Locals {
    PinnedValue<JSObject> proxyHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 5. Set p.[[ProxyTarget]] to null.
  // 6. Set p.[[ProxyHandler]] to null.
  lv.proxyHandle.castAndSetHermesValue<JSObject>(
      HermesValue::encodeObjectValue(proxy));
  JSProxy::setTargetAndHandler(
      lv.proxyHandle,
      runtime,
      runtime.makeNullHandle<JSObject>(),
      runtime.makeNullHandle<JSObject>());
  // 7. Return undefined.
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue> proxyRevocable(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();

  struct : public Locals {
    PinnedValue<JSObject> initialProxy;
    PinnedValue<JSObject> finalProxy;
    PinnedValue<JSObject> result;
    PinnedValue<NativeFunction> revoker;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // 1. Let p be ? ProxyCreate(target, handler).
  lv.initialProxy = vm::JSProxy::create(runtime);
  auto status = proxyCreate(
      runtime,
      args.dyncastArg<JSObject>(0),
      args.dyncastArg<JSObject>(1),
      lv.initialProxy,
      MutableHandle{lv.finalProxy});
  if (status == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  // 2. Let steps be the algorithm steps defined in Proxy Revocation Functions.
  // 3. Let revoker be CreateBuiltinFunction(steps, « [[RevocableProxy]] »).
  static_assert(
      ProxySlotIndexes::COUNT == 1,
      "COUNT must be 1. "
      "Only 1 reserved slot in classNativeFunction1Reserved");
  lv.revoker = NativeFunction::create(
      runtime,
      runtime.functionPrototype,
      runtime.classNativeFunction1Reserved,
      Runtime::makeNullHandle<Environment>(),
      nullptr,
      proxyRevocationSteps,
      Predefined::getSymbolID(Predefined::emptyString),
      0,
      Runtime::makeNullHandle<JSObject>());
  // 4. Set revoker.[[RevocableProxy]] to p.
  auto shv = SmallHermesValue::encodeHermesValue(
      lv.finalProxy.getHermesValue(), runtime);
  setRevocableProxySlot(*lv.revoker, runtime, shv);
  // 5. Let result be ObjectCreate(%ObjectPrototype%).
  lv.result = JSObject::create(runtime);
  // 6. Perform CreateDataProperty(result, "proxy", p).
  auto res1 = JSObject::putNamed_RJS(
      lv.result,
      runtime,
      Predefined::getSymbolID(Predefined::proxy),
      lv.finalProxy);
  if (res1 == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  assert(*res1 && "Failed to set proxy on Proxy.revocable return");
  // 7. Perform CreateDataProperty(result, "revoke", revoker).
  auto res2 = JSObject::putNamed_RJS(
      lv.result,
      runtime,
      Predefined::getSymbolID(Predefined::revoke),
      lv.revoker);
  if (res2 == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  assert(*res2 && "Failed to set revoke on Proxy.revocable return");
  // 8. Return result.
  return lv.result.getHermesValue();
}

HermesValue createProxyConstructor(Runtime &runtime) {
  struct : public Locals {
    PinnedValue<NativeConstructor> cons;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  defineSystemConstructor(
      runtime,
      Predefined::getSymbolID(Predefined::Proxy),
      proxyConstructor,
      runtime.makeNullHandle<JSObject>(),
      2,
      lv.cons);

  defineMethod(
      runtime,
      lv.cons,
      Predefined::getSymbolID(Predefined::revocable),
      nullptr,
      proxyRevocable,
      2);

  return lv.cons.getHermesValue();
}

} // namespace vm
} // namespace hermes

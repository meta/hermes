/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// ES5.1 15.12 Populate the JSON object.
//===----------------------------------------------------------------------===//
#include "JSLibInternal.h"
#include "hermes/VM/JSLib/RuntimeJSONParse.h"
#include "hermes/VM/JSLib/RuntimeJSONStringify.h"
#include "hermes/VM/SingleObject.h"
#include "hermes/VM/StringPrimitive.h"

namespace hermes {
namespace vm {

void createJSONObject(Runtime &runtime, MutableHandle<JSObject> result) {
  auto objRes = JSJSON::create(runtime);
  assert(
      objRes != ExecutionStatus::EXCEPTION && "unable to define JSON object");

  struct : public Locals {
    PinnedValue<JSJSON> json;
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.json.castAndSetHermesValue<JSJSON>(*objRes);

  defineMethod(
      runtime,
      lv.json,
      Predefined::getSymbolID(Predefined::parse),
      nullptr,
      jsonParse,
      2);
  defineMethod(
      runtime,
      lv.json,
      Predefined::getSymbolID(Predefined::stringify),
      nullptr,
      jsonStringify,
      3);

  auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  lv.str = runtime.getPredefinedString(Predefined::JSON);
  defineProperty(
      runtime,
      lv.json,
      Predefined::getSymbolID(Predefined::SymbolToStringTag),
      lv.str,
      dpf);

  result.set(lv.json.get());
}

CallResult<HermesValue> jsonParse(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  auto res = toString_RJS(runtime, args.getArgHandle(0));
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  struct : public Locals {
    PinnedValue<StringPrimitive> strHandle;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.strHandle.castAndSetHermesValue<StringPrimitive>(res->getHermesValue());
  return runtimeJSONParse(
      runtime,
      lv.strHandle,
      Handle<Callable>::dyn_vmcast(args.getArgHandle(1)));
}

CallResult<HermesValue> jsonStringify(void *, Runtime &runtime) {
  NativeArgs args = runtime.getCurrentFrame().getNativeArgs();
  return runtimeJSONStringify(
      runtime,
      args.getArgHandle(0),
      args.getArgHandle(1),
      args.getArgHandle(2));
}

} // namespace vm
} // namespace hermes

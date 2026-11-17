/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSONValue.h"

#include "JSLib/Object.h"
#include "hermes/Support/UTF8.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/StringView.h"

#include <cmath>
#include <string>
#include <vector>

namespace hermes {
namespace vm {
namespace {

JSONValue jsonTreeStringToJSONValue(
    Runtime &runtime,
    Handle<StringPrimitive> str) {
  auto view = StringPrimitive::createStringView(runtime, str);
  if (view.isASCII()) {
    return JSONValue::asciiString(
        std::string{view.castToCharPtr(), view.length()});
  }

  return JSONValue::utf16String(
      std::u16string{view.castToChar16Ptr(), view.length()});
}

std::string jsonTreeStringToUTF8(
    Runtime &runtime,
    Handle<StringPrimitive> str) {
  auto view = StringPrimitive::createStringView(runtime, str);
  if (view.isASCII()) {
    return std::string{view.castToCharPtr(), view.length()};
  }

  std::string result;
  ::hermes::convertUTF16ToUTF8WithReplacements(
      result, llvh::ArrayRef{view.castToChar16Ptr(), view.length()});
  return result;
}

bool jsonTreeContainsAncestor(
    const std::vector<Handle<JSObject>> &ancestors,
    Handle<JSObject> object) {
  for (auto ancestor : ancestors) {
    if (*ancestor == *object) {
      return true;
    }
  }
  return false;
}

class JSONTreeAncestorScope {
 public:
  JSONTreeAncestorScope(
      std::vector<Handle<JSObject>> &ancestors,
      Handle<JSObject> object)
      : ancestors_(ancestors) {
    ancestors_.push_back(object);
  }

  ~JSONTreeAncestorScope() {
    ancestors_.pop_back();
  }

 private:
  std::vector<Handle<JSObject>> &ancestors_;
};

CallResult<JSONValue> createJSONValueFromHermesValueImpl(
    Runtime &runtime,
    Handle<> value,
    std::vector<Handle<JSObject>> &ancestors) {
  if (value->isUndefined() || value->isNull()) {
    return JSONValue::null();
  }
  if (value->isBool()) {
    return JSONValue::boolean(value->getBool());
  }
  if (value->isNumber()) {
    const double number = value->getNumber();
    return std::isfinite(number) ? JSONValue::number(number)
                                 : JSONValue::null();
  }
  if (value->isString()) {
    return jsonTreeStringToJSONValue(
        runtime, Handle<StringPrimitive>::vmcast(value));
  }
  if (value->isSymbol()) {
    return runtime.raiseTypeError("Cannot create JSONValue from symbol");
  }
  if (value->isBigInt()) {
    return runtime.raiseTypeError("Cannot create JSONValue from bigint");
  }

  assert(value->isObject() && "Unhandled HermesValue kind");

  struct : public Locals {
    PinnedValue<JSObject> object;
    PinnedValue<JSArray> keys;
    PinnedValue<> key;
    PinnedValue<> child;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  lv.object = vmcast<JSObject>(*value);
  if (vmisa<Callable>(*lv.object)) {
    return runtime.raiseTypeError("Cannot create JSONValue from function");
  }
  if (jsonTreeContainsAncestor(ancestors, lv.object)) {
    return runtime.raiseTypeError("Cannot create JSONValue from cyclic object");
  }

  JSONTreeAncestorScope ancestorScope{ancestors, lv.object};

  Handle<JSObject> objectHandle = lv.object;
  if (auto array = Handle<JSArray>::dyn_vmcast(objectHandle)) {
    const uint32_t len = JSArray::getLength(*array, runtime);
    std::vector<JSONValue> result;
    result.reserve(len);

    for (uint32_t i = 0; i < len; ++i) {
      GCScopeMarkerRAII marker{runtime};
      auto element = array->at(runtime, i);
      if (element.isEmpty()) {
        result.push_back(JSONValue::null());
        continue;
      }

      lv.child = element.unboxToHV(runtime);
      auto childRes =
          createJSONValueFromHermesValueImpl(runtime, lv.child, ancestors);
      if (childRes == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      result.push_back(std::move(*childRes));
    }

    return JSONValue::array(std::move(result));
  }

  auto keysRes = enumerableOwnProperties_RJS(
      runtime, lv.object, EnumerableOwnPropertiesKind::Key);
  if (keysRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  lv.keys.castAndSetHermesValue<JSArray>(*keysRes);

  const uint32_t len = JSArray::getLength(*lv.keys, runtime);
  JSONValue::Object result;
  result.reserve(len);

  for (uint32_t i = 0; i < len; ++i) {
    GCScopeMarkerRAII marker{runtime};
    auto key = lv.keys->at(runtime, i);
    if (key.isEmpty()) {
      continue;
    }
    lv.key = key.unboxToHV(runtime);
    if (!lv.key->isString()) {
      return runtime.raiseTypeError("Enumerable object key is not a string");
    }

    auto propRes = JSObject::getComputed_RJS(lv.object, runtime, lv.key);
    if (propRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.child = std::move(*propRes);

    auto childRes =
        createJSONValueFromHermesValueImpl(runtime, lv.child, ancestors);
    if (childRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }

    result.emplace_back(
        jsonTreeStringToUTF8(
            runtime, Handle<StringPrimitive>::vmcast(&lv.key)),
        std::move(*childRes));
  }

  return JSONValue::object(std::move(result));
}

} // namespace

CallResult<JSONValue> createJSONValueFromHermesValue(
    Runtime &runtime,
    Handle<> value) {
  std::vector<Handle<JSObject>> ancestors;
  return createJSONValueFromHermesValueImpl(runtime, value, ancestors);
}

} // namespace vm
} // namespace hermes

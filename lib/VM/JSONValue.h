/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "hermes/VM/Handle.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/SmallHermesValue-inline.h"
#include "hermes/VM/StringPrimitive.h"
#include "jsi/jsi.h"

#include "llvh/ADT/ArrayRef.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hermes::vm {

using JSONValue = ::facebook::jsi::JSONValue;

/// Create a JSONValue tree from a Hermes value.
///
/// This walks arrays and own enumerable object properties. Values that do not
/// have a JSONValue representation, such as functions, symbols, bigints, and
/// cyclic objects, throw.
CallResult<JSONValue> createJSONValueFromHermesValue(
    Runtime &runtime,
    Handle<> value);

/// Materializes a JSONValue tree directly into Hermes VM values.
///
/// This intentionally bypasses Hermes' StructuredDeserialize format. It is
/// useful when native code already has a decoded JSON-like C++ tree and wants
/// to construct the JS graph with less generic structured-clone overhead.
class JSONValueMaterializer {
 public:
  /// Materialize \p value without mutating it.
  CallResult<HermesValue> materialize(
      Runtime &runtime,
      const JSONValue &value) {
    MaterializationContext context{/*consumeStringValues*/ false, {}};
    return materializeImpl(runtime, const_cast<JSONValue &>(value), context);
  }

  /// Materialize \p value and allow string storage to be moved into Hermes.
  ///
  /// After this returns, \p value must be treated as consumed. This is the safe
  /// ownership-transfer version of "reuse C string memory": Hermes may adopt
  /// moved std::string storage for ASCII strings and moved std::u16string
  /// storage for UTF-16 strings. UTF-8 strings still go through Hermes' UTF-8
  /// decoder.
  CallResult<HermesValue> materializeAndConsume(
      Runtime &runtime,
      JSONValue &value) {
    MaterializationContext context{/*consumeStringValues*/ true, {}};
    return materializeImpl(runtime, value, context);
  }

 private:
  struct MaterializationContext {
    bool consumeStringValues;
    std::unordered_map<std::string, SymbolID> keySymbolCache;
  };

  static CallResult<HermesValue> createString(
      Runtime &runtime,
      JSONValue &value,
      bool consume) {
    switch (value.stringEncoding) {
      case JSONValue::StringEncoding::ASCII:
        if (consume) {
          return StringPrimitive::createEfficient(
              runtime, std::move(value.stringValue));
        }
        return StringPrimitive::createEfficient(
            runtime,
            ASCIIRef{value.stringValue.data(), value.stringValue.size()});
      case JSONValue::StringEncoding::UTF8: {
        const auto *data =
            reinterpret_cast<const uint8_t *>(value.stringValue.data());
        return StringPrimitive::createEfficient(
            runtime, UTF8Ref{data, value.stringValue.size()});
      }
      case JSONValue::StringEncoding::UTF16:
        if (consume) {
          return StringPrimitive::createEfficient(
              runtime, std::move(value.utf16StringValue));
        }
        return StringPrimitive::createEfficient(
            runtime,
            UTF16Ref{
                value.utf16StringValue.data(),
                value.utf16StringValue.size()});
    }
    assert(false && "Unhandled JSON string encoding");
    return HermesValue::encodeUndefinedValue();
  }

  static CallResult<HermesValue> createStringFromUTF8(
      Runtime &runtime,
      const std::string &value) {
    if (JSONValue::isASCII(value)) {
      return StringPrimitive::createEfficient(
          runtime, ASCIIRef{value.data(), value.size()});
    }

    const auto *data = reinterpret_cast<const uint8_t *>(value.data());
    return StringPrimitive::createEfficient(
        runtime, UTF8Ref{data, value.size()});
  }

  CallResult<HermesValue> materializeImpl(
      Runtime &runtime,
      JSONValue &value,
      MaterializationContext &context) {
    switch (value.kind) {
      case JSONValue::Kind::Null:
        return HermesValue::encodeNullValue();
      case JSONValue::Kind::Bool:
        return HermesValue::encodeBoolValue(value.boolValue);
      case JSONValue::Kind::Number:
        if (!std::isfinite(value.numberValue)) {
          return runtime.raiseRangeError("JSON numbers must be finite");
        }
        return HermesValue::encodeUntrustedNumberValue(value.numberValue);
      case JSONValue::Kind::String:
        return createString(runtime, value, context.consumeStringValues);
      case JSONValue::Kind::Array:
        return materializeArray(runtime, value.arrayValue, context);
      case JSONValue::Kind::Object:
        return materializeObject(runtime, value.objectValue, context);
    }
    assert(false && "Unhandled JSON kind");
    return HermesValue::encodeUndefinedValue();
  }

  CallResult<HermesValue> materializeArray(
      Runtime &runtime,
      std::vector<JSONValue> &array,
      MaterializationContext &context) {
    if (array.size() > std::numeric_limits<uint32_t>::max()) {
      return runtime.raiseRangeError("JSON array is too large");
    }

    struct : Locals {
      PinnedValue<JSArray> array;
      PinnedValue<> element;
    } lv;
    LocalsRAII lraii{runtime, &lv};

    const auto len = static_cast<uint32_t>(array.size());
    auto arrayRes = JSArray::create(runtime, len, len);
    if (arrayRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    lv.array = std::move(*arrayRes);

    for (uint32_t i = 0; i < len; ++i) {
      auto elementRes = materializeImpl(runtime, array[i], context);
      if (elementRes == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.element = *elementRes;

      auto shv = SmallHermesValue::encodeHermesValue(
          lv.element.getHermesValue(), runtime);
      JSArray::unsafeSetExistingElementAt(*lv.array, runtime, i, shv);
    }

    return lv.array.getHermesValue();
  }

  CallResult<HermesValue> materializeObject(
      Runtime &runtime,
      JSONValue::Object &object,
      MaterializationContext &context) {
    if (object.size() > std::numeric_limits<unsigned>::max()) {
      return runtime.raiseRangeError("JSON object has too many properties");
    }

    struct : Locals {
      PinnedValue<JSObject> object;
      PinnedValue<StringPrimitive> keyString;
      PinnedValue<SymbolID> keySymbol;
      PinnedValue<> value;
    } lv;
    LocalsRAII lraii{runtime, &lv};

    lv.object = JSObject::create(runtime, static_cast<unsigned>(object.size()));

    for (auto &[key, child] : object) {
      auto cachedSymbol = context.keySymbolCache.find(key);
      if (cachedSymbol != context.keySymbolCache.end()) {
        lv.keySymbol = cachedSymbol->second;
      } else {
        auto keyRes = createStringFromUTF8(runtime, key);
        if (keyRes == ExecutionStatus::EXCEPTION) {
          return ExecutionStatus::EXCEPTION;
        }
        if (!keyRes->isString()) {
          return runtime.raiseTypeError("JSON object key is not a string");
        }
        lv.keyString = keyRes->getString();

        auto symbolRes =
            stringToSymbolID(runtime, createPseudoHandle(*lv.keyString));
        if (symbolRes == ExecutionStatus::EXCEPTION) {
          return ExecutionStatus::EXCEPTION;
        }
        lv.keySymbol = std::move(*symbolRes);
        context.keySymbolCache.emplace(key, *lv.keySymbol);
      }

      auto valueRes = materializeImpl(runtime, child, context);
      if (valueRes == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      lv.value = *valueRes;

      auto status = JSObject::defineNewOwnProperty(
          lv.object,
          runtime,
          *lv.keySymbol,
          PropertyFlags().defaultNewNamedPropertyFlags(),
          lv.value);
      if (status == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
    }

    return lv.object.getHermesValue();
  }
};

} // namespace hermes::vm

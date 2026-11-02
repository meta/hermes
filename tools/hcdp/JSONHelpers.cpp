/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSONHelpers.h"

#include <hermes/Parser/JSONParser.h>
#include <hermes/cdp/JSONValueInterfaces.h> // @manual=fbsource//xplat/hermes/API:HermesAPI
#include <hermes/cdp/MessageTypesInlines.h> // @manual=fbsource//xplat/hermes/API:HermesAPI

namespace hermes {

using namespace ::hermes::parser;
using namespace ::facebook::hermes::cdp;
using namespace ::facebook::hermes::cdp::message;

std::optional<long long> getResponseId(const std::string &message) {
  JSLexer::Allocator allocator;
  JSONFactory factory(allocator);
  std::optional<JSONObject *> obj = parseStrAsJsonObj(message, factory);
  if (!obj.has_value()) {
    return std::nullopt;
  }

  JSONValue *v = obj.value()->get("id");
  if (v == nullptr) {
    return std::nullopt;
  }

  std::unique_ptr<long long> id = valueFromJson<long long>(v);
  if (id == nullptr) {
    return std::nullopt;
  }
  return *id;
}

} // namespace hermes

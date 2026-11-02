/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
/// Benchmark JSON.parse-equivalent materialization against Hermes
/// ISerialization deserialization for the same JSON-shaped data.
//===----------------------------------------------------------------------===//

#include "hermes/Support/OSCompat.h"
#include "hermes/Support/UTF16Stream.h"
#include "hermes/VM/Casting.h"
#include "hermes/VM/Handle.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSLib/RuntimeJSONParse.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/SerializedValue.h"
#include "hermes/hermes.h"

#include "JSONSerializedValueEncoder.h"
#include "JSONValue.h"

#include "llvh/Support/CommandLine.h"
#include "llvh/Support/InitLLVM.h"
#include "llvh/Support/PrettyStackTrace.h"
#include "llvh/Support/Signals.h"
#include "llvh/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fbhermes = facebook::hermes;
namespace jsi = facebook::jsi;
namespace ser = hermes::vm;
namespace vm = ::hermes::vm;

namespace {

static llvh::cl::opt<uint32_t> Iterations{
    "iterations",
    llvh::cl::init(1000),
    llvh::cl::desc("Number of timed iterations per benchmark")};

static llvh::cl::opt<uint32_t> Warmup{
    "warmup",
    llvh::cl::init(100),
    llvh::cl::desc("Number of untimed warmup iterations per benchmark")};

static llvh::cl::opt<uint32_t> Items{
    "items",
    llvh::cl::init(1000),
    llvh::cl::desc("Number of objects in the generated JSON array")};

static llvh::cl::opt<uint32_t> ValuesPerItem{
    "values",
    llvh::cl::init(8),
    llvh::cl::desc("Values per item, or nesting depth for --shape=deep")};

static llvh::cl::opt<uint32_t> ValidateRepetitions{
    "validate-repetitions",
    llvh::cl::init(0),
    llvh::cl::desc("Run cross-runtime validation repeatedly and exit")};

static llvh::cl::opt<std::string> Shape{
    "shape",
    llvh::cl::init("mixed"),
    llvh::cl::desc("Payload shape: strings, unicode, numbers, mixed, deep")};

static volatile uint64_t Sink;

struct BenchPayload {
  std::string json;
  ser::JSONValue value;
  uint32_t rootLength;
};

BenchPayload makeRecordsPayload(uint32_t items, uint32_t valuesPerItem) {
  std::ostringstream os;
  std::vector<ser::JSONValue> array;
  array.reserve(items);

  os << '[';
  for (uint32_t i = 0; i < items; ++i) {
    if (i != 0) {
      os << ',';
    }
    os << "{\"id\":" << i << ",\"name\":\"item-" << i
       << "\",\"enabled\":" << ((i % 2) == 0 ? "true" : "false")
       << ",\"nested\":{\"group\":" << (i % 17)
       << ",\"label\":\"group-" << (i % 17) << "\"},\"values\":[";
    for (uint32_t j = 0; j < valuesPerItem; ++j) {
      if (j != 0) {
        os << ',';
      }
      os << (i * valuesPerItem + j);
    }
    os << "]}";

    std::vector<ser::JSONValue> values;
    values.reserve(valuesPerItem);
    for (uint32_t j = 0; j < valuesPerItem; ++j) {
      values.push_back(ser::JSONValue::number(i * valuesPerItem + j));
    }

    ser::JSONValue::Object nested;
    nested.reserve(2);
    nested.emplace_back("group", ser::JSONValue::number(i % 17));
    nested.emplace_back(
        "label", ser::JSONValue::string("group-" + std::to_string(i % 17)));

    ser::JSONValue::Object object;
    object.reserve(5);
    object.emplace_back("id", ser::JSONValue::number(i));
    object.emplace_back(
        "name", ser::JSONValue::string("item-" + std::to_string(i)));
    object.emplace_back("enabled", ser::JSONValue::boolean((i % 2) == 0));
    object.emplace_back("nested", ser::JSONValue::object(std::move(nested)));
    object.emplace_back("values", ser::JSONValue::array(std::move(values)));
    array.push_back(ser::JSONValue::object(std::move(object)));
  }
  os << ']';
  return {os.str(), ser::JSONValue::array(std::move(array)), items};
}

BenchPayload makeNumbersPayload(uint32_t items, uint32_t valuesPerItem) {
  const uint32_t count = items * valuesPerItem;
  std::ostringstream os;
  std::vector<ser::JSONValue> array;
  array.reserve(count);

  os << '[';
  for (uint32_t i = 0; i < count; ++i) {
    if (i != 0) {
      os << ',';
    }
    os << i;
    array.push_back(ser::JSONValue::number(i));
  }
  os << ']';
  return {os.str(), ser::JSONValue::array(std::move(array)), count};
}

BenchPayload makeStringsPayload(uint32_t items, uint32_t valuesPerItem) {
  const uint32_t count = items * valuesPerItem;
  std::ostringstream os;
  std::vector<ser::JSONValue> array;
  array.reserve(count);

  os << '[';
  for (uint32_t i = 0; i < count; ++i) {
    std::string value = "item-" + std::to_string(i) + "-payload-string";
    if (i != 0) {
      os << ',';
    }
    os << '"' << value << '"';
    array.push_back(ser::JSONValue::string(std::move(value)));
  }
  os << ']';
  return {os.str(), ser::JSONValue::array(std::move(array)), count};
}

BenchPayload makeUnicodeStringsPayload(
    uint32_t items,
    uint32_t valuesPerItem) {
  const uint32_t count = items * valuesPerItem;
  std::ostringstream os;
  std::vector<ser::JSONValue> array;
  array.reserve(count);

  os << '[';
  for (uint32_t i = 0; i < count; ++i) {
    std::string asciiPrefix = "item-" + std::to_string(i) + "-";
    std::u16string value{asciiPrefix.begin(), asciiPrefix.end()};
    value.push_back(u'\u00e9');
    value.push_back(u'-');
    value.push_back(u'\u6771');
    value.push_back(u'-');
    value.push_back(0xd83d);
    value.push_back(0xde80);

    if (i != 0) {
      os << ',';
    }
    os << '"' << asciiPrefix << "\\u00e9-\\u6771-\\ud83d\\ude80\"";
    array.push_back(ser::JSONValue::utf16String(std::move(value)));
  }
  os << ']';
  return {os.str(), ser::JSONValue::array(std::move(array)), count};
}

ser::JSONValue makeDeepJSONValue(uint32_t item, uint32_t depth) {
  ser::JSONValue child = ser::JSONValue::number(item);
  for (uint32_t level = 0; level < depth; ++level) {
    ser::JSONValue::Object object;
    object.reserve(2);
    object.emplace_back("level", ser::JSONValue::number(level + 1));
    object.emplace_back("child", std::move(child));
    child = ser::JSONValue::object(std::move(object));
  }
  return child;
}

void appendDeepJSON(std::ostringstream &os, uint32_t item, uint32_t depth) {
  for (uint32_t level = 0; level < depth; ++level) {
    os << "{\"level\":" << (depth - level) << ",\"child\":";
  }
  os << item;
  for (uint32_t level = 0; level < depth; ++level) {
    os << '}';
  }
}

BenchPayload makeDeepPayload(uint32_t items, uint32_t depth) {
  std::ostringstream os;
  std::vector<ser::JSONValue> array;
  array.reserve(items);

  os << '[';
  for (uint32_t i = 0; i < items; ++i) {
    if (i != 0) {
      os << ',';
    }
    appendDeepJSON(os, i, depth);
    array.push_back(makeDeepJSONValue(i, depth));
  }
  os << ']';
  return {os.str(), ser::JSONValue::array(std::move(array)), items};
}

BenchPayload makePayload(
    llvh::StringRef shape,
    uint32_t items,
    uint32_t valuesPerItem) {
  if (shape == "mixed" || shape == "records") {
    return makeRecordsPayload(items, valuesPerItem);
  }
  if (shape == "numbers") {
    return makeNumbersPayload(items, valuesPerItem);
  }
  if (shape == "strings") {
    return makeStringsPayload(items, valuesPerItem);
  }
  if (shape == "unicode") {
    return makeUnicodeStringsPayload(items, valuesPerItem);
  }
  if (shape == "deep") {
    return makeDeepPayload(items, valuesPerItem);
  }
  throw std::invalid_argument("Unknown --shape value");
}

void consumeArrayLength(jsi::Runtime &runtime, const jsi::Value &value) {
  Sink += value.getObject(runtime).getArray(runtime).size(runtime);
}

bool checkGeneratedValue(
    jsi::Runtime &runtime,
    const jsi::Value &value,
    uint32_t rootLength) {
  jsi::Array array = value.getObject(runtime).getArray(runtime);
  return array.size(runtime) == rootLength;
}

std::string stringifyValue(jsi::Runtime &runtime, const jsi::Value &value) {
  jsi::Scope scope{runtime};
  jsi::Object jsonObject =
      runtime.global().getPropertyAsObject(runtime, "JSON");
  jsi::Function stringify =
      jsonObject.getPropertyAsFunction(runtime, "stringify");
  auto callWithArgs = static_cast<jsi::Value (jsi::Function::*)(
      jsi::IRuntime &,
      const jsi::Value *,
      size_t) const>(&jsi::Function::call);
  jsi::Value result = (stringify.*callWithArgs)(runtime, &value, 1);
  return result.asString(runtime).utf8(runtime);
}

bool reportMismatch(
    llvh::StringRef label,
    const std::string &expected,
    const std::string &actual) {
  if (expected == actual) {
    return true;
  }

  const auto expectedPrefixLen = std::min<size_t>(expected.size(), 160);
  const auto actualPrefixLen = std::min<size_t>(actual.size(), 160);
  llvh::errs() << label << " mismatch: expected_bytes=" << expected.size()
               << ", actual_bytes=" << actual.size() << '\n';
  llvh::errs() << "expected_prefix="
               << llvh::StringRef{expected.data(), expectedPrefixLen} << '\n';
  llvh::errs() << "actual_prefix="
               << llvh::StringRef{actual.data(), actualPrefixLen} << '\n';
  return false;
}

bool validateCrossRuntimeRoundTrips(const BenchPayload &benchPayload) {
  auto sourceRuntime = fbhermes::makeHermesRuntime();
  auto targetRuntime = fbhermes::makeHermesRuntime();
  jsi::Scope sourceScope{*sourceRuntime};
  jsi::Scope targetScope{*targetRuntime};

  auto *sourceSerialization =
      jsi::castInterface<jsi::ISerialization>(sourceRuntime.get());
  auto *targetSerialization =
      jsi::castInterface<jsi::ISerialization>(targetRuntime.get());
  auto *sourceJSONFactory =
      jsi::castInterface<jsi::IJSONValueFactory>(sourceRuntime.get());
  auto *targetJSONFactory =
      jsi::castInterface<jsi::IJSONValueFactory>(targetRuntime.get());
  auto *targetTracingHelpers =
      jsi::castInterface<fbhermes::IHermesTracingHelpers>(
          targetRuntime.get());

  if (!sourceSerialization || !targetSerialization || !sourceJSONFactory ||
      !targetJSONFactory || !targetTracingHelpers) {
    llvh::errs() << "Runtime does not expose validation interfaces.\n";
    return false;
  }

  const std::string &json = benchPayload.json;
  jsi::Value sourceParsed = sourceRuntime->createValueFromJsonUtf8(
      reinterpret_cast<const uint8_t *>(json.data()), json.size());
  const std::string expected = stringifyValue(*sourceRuntime, sourceParsed);

  auto structuredSerialized = sourceSerialization->serialize(sourceParsed);
  jsi::Value structuredTarget =
      targetSerialization->deserialize(structuredSerialized);
  if (!reportMismatch(
          "ISerialization cross-runtime",
          expected,
          stringifyValue(*targetRuntime, structuredTarget))) {
    return false;
  }

  jsi::JSONValue extractedTree =
      sourceJSONFactory->createJSONTreeFromValue(sourceParsed);
  jsi::Value treeTarget =
      targetJSONFactory->createValueFromJSONTree(extractedTree);
  if (!reportMismatch(
          "JSI JSON tree cross-runtime",
          expected,
          stringifyValue(*targetRuntime, treeTarget))) {
    return false;
  }

  ser::JSONSerializedValueEncoder encoder;
  vm::SerializedValue customPayload = encoder.encode(benchPayload.value);
  auto customSerialized = targetTracingHelpers->makeSerialized(customPayload);
  jsi::Value customTarget = targetSerialization->deserialize(customSerialized);
  if (!reportMismatch(
          "Runtime-free serialized JSON tree",
          expected,
          stringifyValue(*targetRuntime, customTarget))) {
    return false;
  }

  return true;
}

template <typename Func>
double timeBenchmark(
    jsi::Runtime &runtime,
    uint32_t warmup,
    uint32_t iterations,
    Func &&func) {
  for (uint32_t i = 0; i < warmup; ++i) {
    consumeArrayLength(runtime, func());
  }

  auto begin = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < iterations; ++i) {
    consumeArrayLength(runtime, func());
  }
  auto end = std::chrono::steady_clock::now();

  return std::chrono::duration<double, std::milli>(end - begin).count();
}

uint32_t jsonTreeRootLength(const jsi::JSONValue &value) {
  return value.kind == jsi::JSONValue::Kind::Array
      ? static_cast<uint32_t>(value.arrayValue.size())
      : 0;
}

template <typename Func>
double timeJSONTreeBenchmark(
    uint32_t warmup,
    uint32_t iterations,
    Func &&func) {
  for (uint32_t i = 0; i < warmup; ++i) {
    Sink += jsonTreeRootLength(func());
  }

  auto begin = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < iterations; ++i) {
    Sink += jsonTreeRootLength(func());
  }
  auto end = std::chrono::steady_clock::now();

  return std::chrono::duration<double, std::milli>(end - begin).count();
}

template <typename Func>
double timeSerializedBenchmark(
    const fbhermes::IHermesTracingHelpers &tracingHelpers,
    uint32_t warmup,
    uint32_t iterations,
    Func &&func) {
  for (uint32_t i = 0; i < warmup; ++i) {
    auto serialized = func();
    const auto *payload = tracingHelpers.getHermesSerializedValue(*serialized);
    Sink += payload->content.size();
  }

  auto begin = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < iterations; ++i) {
    auto serialized = func();
    const auto *payload = tracingHelpers.getHermesSerializedValue(*serialized);
    Sink += payload->content.size();
  }
  auto end = std::chrono::steady_clock::now();

  return std::chrono::duration<double, std::milli>(end - begin).count();
}

void consumeVMArrayLength(vm::Runtime &runtime, vm::HermesValue value) {
  Sink += vm::JSArray::getLength(
      vm::vmcast<vm::JSArray>(value.getObject(runtime)), runtime);
}

template <typename Func>
double timeVMBenchmark(
    vm::Runtime &runtime,
    uint32_t warmup,
    uint32_t iterations,
    Func &&func) {
  struct : vm::Locals {
    vm::PinnedValue<> value;
  } lv;
  vm::LocalsRAII lraii{runtime, &lv};

  for (uint32_t i = 0; i < warmup; ++i) {
    vm::GCScopeMarkerRAII marker{runtime};
    auto result = func();
    if (result == vm::ExecutionStatus::EXCEPTION) {
      throw std::runtime_error("VM benchmark warmup threw");
    }
    lv.value = *result;
    consumeVMArrayLength(runtime, lv.value.getHermesValue());
  }

  auto begin = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < iterations; ++i) {
    vm::GCScopeMarkerRAII marker{runtime};
    auto result = func();
    if (result == vm::ExecutionStatus::EXCEPTION) {
      throw std::runtime_error("VM benchmark iteration threw");
    }
    lv.value = *result;
    consumeVMArrayLength(runtime, lv.value.getHermesValue());
  }
  auto end = std::chrono::steady_clock::now();

  return std::chrono::duration<double, std::milli>(end - begin).count();
}

#ifdef JSI_UNSTABLE
vm::SerializedValue clonePayload(const vm::SerializedValue &source) {
  vm::SerializedValue clone;
  clone.offsets = source.offsets;
  clone.content = source.content;
  clone.strings = source.strings;
  return clone;
}
#endif

void printResult(llvh::StringRef name, double ms, uint32_t iterations) {
  llvh::outs() << name << ": total=" << ms << " ms, per_iter="
               << (ms * 1000.0 / iterations) << " us, ops_per_sec="
               << (iterations * 1000.0 / ms) << '\n';
}

} // namespace

int main(int argc, char **argv) {
  llvh::InitLLVM initLLVM(argc, argv);
  llvh::sys::PrintStackTraceOnErrorSignal("serialization-json-bench");
  llvh::PrettyStackTraceProgram X(argc, argv);
  llvh::llvm_shutdown_obj Y;
  llvh::cl::ParseCommandLineOptions(
      argc, argv, "Hermes JSON parse vs ISerialization benchmark\n");

  hermes::oscompat::SigAltStackLeakSuppressor sigAltLeakSuppressor;

#ifndef JSI_UNSTABLE
  llvh::errs() << "This benchmark requires JSI_UNSTABLE.\n";
  return EXIT_FAILURE;
#else
  auto runtime = fbhermes::makeHermesRuntime();
  auto *serialization =
      jsi::castInterface<jsi::ISerialization>(runtime.get());
  auto *jsonFactory =
      jsi::castInterface<jsi::IJSONValueFactory>(runtime.get());
  auto *tracingHelpers =
      jsi::castInterface<fbhermes::IHermesTracingHelpers>(runtime.get());

  if (!serialization || !jsonFactory || !tracingHelpers) {
    llvh::errs() << "Runtime does not expose serialization interfaces.\n";
    return EXIT_FAILURE;
  }

  BenchPayload benchPayload = makePayload(Shape, Items, ValuesPerItem);
  if (ValidateRepetitions != 0) {
    for (uint32_t i = 0; i < ValidateRepetitions; ++i) {
      if (!validateCrossRuntimeRoundTrips(benchPayload)) {
        return EXIT_FAILURE;
      }
    }
    llvh::outs() << "validate_repetitions=" << ValidateRepetitions
                 << ", shape=" << Shape << ", items=" << Items
                 << ", values_per_item=" << ValuesPerItem << '\n';
    return EXIT_SUCCESS;
  }

  if (!validateCrossRuntimeRoundTrips(benchPayload)) {
    return EXIT_FAILURE;
  }

  const std::string &json = benchPayload.json;
  jsi::Value parsed = runtime->createValueFromJsonUtf8(
      reinterpret_cast<const uint8_t *>(json.data()), json.size());
  auto serialized = serialization->serialize(parsed);
  const vm::SerializedValue *payload =
      tracingHelpers->getHermesSerializedValue(*serialized);

  ser::JSONSerializedValueEncoder encoder;
  vm::SerializedValue customPayload = encoder.encode(benchPayload.value);
  auto customSerialized = tracingHelpers->makeSerialized(customPayload);
  const vm::SerializedValue *customPayloadView =
      tracingHelpers->getHermesSerializedValue(*customSerialized);
  ser::JSONValueMaterializer materializer;

  jsi::Value customDeserialized =
      serialization->deserialize(customSerialized);
  if (!checkGeneratedValue(
          *runtime, customDeserialized, benchPayload.rootLength)) {
    llvh::errs() << "Custom serializer produced an unexpected value.\n";
    return EXIT_FAILURE;
  }
  jsi::Value jsonTreeValue =
      jsonFactory->createValueFromJSONTree(benchPayload.value);
  if (!checkGeneratedValue(*runtime, jsonTreeValue, benchPayload.rootLength)) {
    llvh::errs() << "JSON tree materializer produced an unexpected value.\n";
    return EXIT_FAILURE;
  }
  jsi::JSONValue extractedTree =
      jsi::JSONValue::createFromValue(*runtime, parsed);
  jsi::Value extractedTreeValue =
      jsonFactory->createValueFromJSONTree(extractedTree);
  if (!checkGeneratedValue(
          *runtime, extractedTreeValue, benchPayload.rootLength)) {
    llvh::errs() << "JSON tree extractor produced an unexpected value.\n";
    return EXIT_FAILURE;
  }

  llvh::outs() << "payload: json_bytes=" << json.size()
               << ", offsets=" << payload->offsets.size()
               << ", content_bytes=" << payload->content.size()
               << ", strings_bytes=" << payload->strings.size() << '\n';
  llvh::outs() << "custom_payload: offsets="
               << customPayloadView->offsets.size()
               << ", content_bytes=" << customPayloadView->content.size()
               << ", strings_bytes=" << customPayloadView->strings.size()
               << '\n';
  llvh::outs() << "iterations=" << Iterations << ", warmup=" << Warmup
               << ", shape=" << Shape << ", items=" << Items
               << ", values_per_item=" << ValuesPerItem << '\n';

  double jsonMs = timeBenchmark(
      *runtime, Warmup, Iterations, [&]() -> jsi::Value {
        return runtime->createValueFromJsonUtf8(
            reinterpret_cast<const uint8_t *>(json.data()), json.size());
      });

  double serializedSerializeMs = timeSerializedBenchmark(
      *tracingHelpers, Warmup, Iterations, [&]() {
        return serialization->serialize(parsed);
      });

  double jsonTreeExtractMs = timeJSONTreeBenchmark(
      Warmup, Iterations, [&]() -> jsi::JSONValue {
        return jsi::JSONValue::createFromValue(*runtime, parsed);
      });

  double deserializeMs = timeBenchmark(
      *runtime, Warmup, Iterations, [&]() -> jsi::Value {
        return serialization->deserialize(serialized);
      });

  double cloneAndDeserializeMs = timeBenchmark(
      *runtime, Warmup, Iterations, [&]() -> jsi::Value {
        vm::SerializedValue clone = clonePayload(*payload);
        auto wrapped = tracingHelpers->makeSerialized(clone);
        return serialization->deserialize(wrapped);
      });

  double customDeserializeMs = timeBenchmark(
      *runtime, Warmup, Iterations, [&]() -> jsi::Value {
        return serialization->deserialize(customSerialized);
      });

  double customCloneAndDeserializeMs = timeBenchmark(
      *runtime, Warmup, Iterations, [&]() -> jsi::Value {
        vm::SerializedValue clone = clonePayload(*customPayloadView);
        auto wrapped = tracingHelpers->makeSerialized(clone);
        return serialization->deserialize(wrapped);
      });

  double jsonTreeMaterializeMs = timeBenchmark(
      *runtime, Warmup, Iterations, [&]() -> jsi::Value {
        return jsonFactory->createValueFromJSONTree(benchPayload.value);
      });

  double jsonTreeExtractMaterializeMs = timeBenchmark(
      *runtime, Warmup, Iterations, [&]() -> jsi::Value {
        jsi::JSONValue tree = jsi::JSONValue::createFromValue(*runtime, parsed);
        return jsonFactory->createValueFromJSONTree(tree);
      });

  double serializedRoundTripMs = timeBenchmark(
      *runtime, Warmup, Iterations, [&]() -> jsi::Value {
        auto nextSerialized = serialization->serialize(parsed);
        return serialization->deserialize(nextSerialized);
      });

  auto vmRuntime = vm::Runtime::create(vm::RuntimeConfig::Builder().build());
  vm::GCScope vmGCScope{*vmRuntime};
  struct : vm::Locals {
    vm::PinnedValue<> checkValue;
  } vmLv;
  vm::LocalsRAII vmLRAII{*vmRuntime, &vmLv};

  llvh::ArrayRef<uint8_t> jsonRef{
      reinterpret_cast<const uint8_t *>(json.data()), json.size()};
  auto vmCheckRes = materializer.materialize(*vmRuntime, benchPayload.value);
  if (vmCheckRes == vm::ExecutionStatus::EXCEPTION) {
    llvh::errs() << "JSON tree materializer produced an unexpected value.\n";
    return EXIT_FAILURE;
  }
  vmLv.checkValue = *vmCheckRes;
  if (!vmLv.checkValue->isObject() ||
      vm::JSArray::getLength(
          vm::vmcast<vm::JSArray>(
              vmLv.checkValue.getHermesValue().getObject(*vmRuntime)),
          *vmRuntime) !=
          benchPayload.rootLength) {
    llvh::errs() << "JSON tree materializer produced an unexpected value.\n";
    return EXIT_FAILURE;
  }

  double vmJsonMs = timeVMBenchmark(
      *vmRuntime, Warmup, Iterations, [&]() -> vm::CallResult<vm::HermesValue> {
        return vm::runtimeJSONParseRef(
            *vmRuntime, hermes::UTF16Stream(jsonRef));
      });

  double vmTreeMaterializeMs = timeVMBenchmark(
      *vmRuntime, Warmup, Iterations, [&]() -> vm::CallResult<vm::HermesValue> {
        return materializer.materialize(*vmRuntime, benchPayload.value);
      });

  double vmCustomDeserializeMs = timeVMBenchmark(
      *vmRuntime, Warmup, Iterations, [&]() -> vm::CallResult<vm::HermesValue> {
        return vm::deserialize(*vmRuntime, *customPayloadView);
      });

  printResult("json_parse", jsonMs, Iterations);
  printResult("serialized_serialize", serializedSerializeMs, Iterations);
  printResult("jsi_json_tree_extract", jsonTreeExtractMs, Iterations);
  printResult("serialized_deserialize_reuse", deserializeMs, Iterations);
  printResult(
      "serialized_clone_wrap_deserialize", cloneAndDeserializeMs, Iterations);
  printResult("custom_deserialize_reuse", customDeserializeMs, Iterations);
  printResult(
      "custom_clone_wrap_deserialize",
      customCloneAndDeserializeMs,
      Iterations);
  printResult("jsi_json_tree_materialize", jsonTreeMaterializeMs, Iterations);
  printResult(
      "jsi_json_tree_extract_materialize",
      jsonTreeExtractMaterializeMs,
      Iterations);
  printResult("serialized_round_trip", serializedRoundTripMs, Iterations);
  printResult("vm_json_parse", vmJsonMs, Iterations);
  printResult("vm_json_tree_materialize", vmTreeMaterializeMs, Iterations);
  printResult("vm_custom_deserialize", vmCustomDeserializeMs, Iterations);
  llvh::outs() << "sink=" << Sink << '\n';
  return EXIT_SUCCESS;
#endif
}

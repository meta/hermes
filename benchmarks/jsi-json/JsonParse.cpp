/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/hermes.h>
#include <jsi/jsi.h>
#include <jsi/threadsafe.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace jsi = facebook::jsi;
using facebook::hermes::makeHermesRuntime;
using facebook::hermes::makeThreadSafeHermesRuntime;

namespace {

/// Native JSON representations prepared outside the measured region.
struct Input {
  std::string name;
  std::string utf8;
  std::u16string utf16;
  bool ascii;
};

/// One parsing route, including destruction of its returned JSI handle.
struct Method {
  std::string name;
  std::function<jsi::Value()> parse;
};

/// Make inputs in both encodings without measuring transcoding.
std::vector<Input> makeInputs() {
  std::vector<Input> inputs;
  auto addASCII = [&](std::string name, std::string json) {
    inputs.push_back(
        {name, json, std::u16string(json.begin(), json.end()), true});
  };
  addASCII("null", "null");
  addASCII("number", "-12.5e2");
  addASCII("boolean", "true");
  addASCII("small-object", R"({"a":1,"b":true,"c":"hello"})");
  for (size_t length : {16, 128, 4096, 65536}) {
    addASCII(
        "ascii-string-" + std::to_string(length),
        "\"" + std::string(length, 'a') + "\"");
    std::string utf8 = "\"";
    std::u16string utf16 = u"\"";
    // Each repetition is four UTF-16 code units, with a surrogate pair.
    for (size_t i = 0; i < length / 4; ++i) {
      utf8 += "世界😀";
      utf16 += u"世界\U0001F600";
    }
    inputs.push_back(
        {"unicode-string-" + std::to_string(length),
         utf8 + "\"",
         utf16 + u"\"",
         false});
  }
  for (bool ascii : {true, false}) {
    std::string utf8 = "[";
    std::u16string utf16 = u"[";
    for (unsigned i = 0; i < 128; ++i) {
      if (i) {
        utf8 += ',';
        utf16 += u',';
      }
      if (ascii) {
        utf8 += R"({"id":42,"data":{"name":"hello","values":[true,null,1.5]}})";
        utf16 +=
            uR"({"id":42,"data":{"name":"hello","values":[true,null,1.5]}})";
      } else {
        utf8 +=
            R"({"id":42,"data":{"name":"世界😀","values":[true,null,1.5]}})";
        utf16 +=
            uR"({"id":42,"data":{"name":"世界😀","values":[true,null,1.5]}})";
      }
    }
    inputs.push_back(
        {ascii ? "nested-ascii" : "nested-unicode",
         utf8 + "]",
         utf16 + u"]",
         ascii});
  }
  return inputs;
}

/// Keep primitive results observable too. Parsing and handle destruction are
/// already observable through the runtime calls.
volatile size_t sink = 0;

/// Measure synchronous parsing and result destruction, in nanoseconds/op.
double measure(const Method &method, size_t iterations) {
  const auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < iterations; ++i) {
    auto value = method.parse();
    sink += value.isObject() || value.isString() || value.isNull();
  }
  return std::chrono::duration<double, std::nano>(
             std::chrono::steady_clock::now() - start)
             .count() /
      iterations;
}

/// Run all routes in rotated order, with GC outside each measured batch.
void run(const Input &input, bool threadSafe, unsigned rotation) {
  std::unique_ptr<jsi::Runtime> runtime = threadSafe
      ? makeThreadSafeHermesRuntime()
      : std::unique_ptr<jsi::Runtime>(makeHermesRuntime());
  auto &rt = *runtime;
  auto parse = rt.global()
                   .getPropertyAsObject(rt, "JSON")
                   .getPropertyAsFunction(rt, "parse");
  auto prebuilt = jsi::String::createFromUtf16(rt, input.utf16);
  std::vector<Method> methods{
      {"raw-utf16",
       [&] {
         return jsi::Value::createFromJsonUtf16(
             rt, input.utf16.data(), input.utf16.size());
       }},
      {"native-utf8",
       [&] {
         return jsi::Value::createFromJsonUtf8(
             rt,
             reinterpret_cast<const uint8_t *>(input.utf8.data()),
             input.utf8.size());
       }},
      {"cached-parse-utf16",
       [&] {
         return parse.call(rt, jsi::String::createFromUtf16(rt, input.utf16));
       }},
      {"cached-parse-prebuilt", [&] { return parse.call(rt, prebuilt); }}};
  if (input.ascii) {
    methods.push_back({"raw-ascii", [&] {
                         return jsi::Value::createFromJsonASCII(
                             rt, input.utf8.data(), input.utf8.size());
                       }});
    methods.push_back({"cached-parse-ascii", [&] {
                         return parse.call(
                             rt, jsi::String::createFromAscii(rt, input.utf8));
                       }});
  }

  // Calibrate all methods, then use the same iteration count for every route.
  double fastest = 1e30;
  for (const auto &method : methods)
    fastest = std::min(fastest, measure(method, 256));
  const size_t iterations = std::max<size_t>(
      32, std::min<size_t>(1000000, static_cast<size_t>(1e7 / fastest)));
  for (unsigned batch = 0; batch < 11; ++batch) {
    for (size_t i = 0; i < methods.size(); ++i) {
      const auto &method = methods[(i + batch + rotation) % methods.size()];
      rt.instrumentation().collectGarbage("JSON benchmark batch");
      double ns = measure(method, iterations);
      if (batch >= 3) {
        std::cout << input.name << ',' << method.name << ','
                  << input.utf16.size() << ',' << batch - 3 << ',' << iterations
                  << ',' << ns << '\n';
      }
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  bool threadSafe = argc > 1 && std::string(argv[1]) == "thread-safe";
  unsigned rotation = argc > 2 ? std::stoul(argv[2]) : 0;
  std::cout << "input,method,utf16_units,batch,iterations,ns_per_op\n";
  for (const auto &input : makeInputs())
    run(input, threadSafe, rotation);
}

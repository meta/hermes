/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/TraceInterpreter.h>
#include <hermes/hermes.h> // @manual=fbsource//xplat/hermes/API:HermesAPI

#include <gtest/gtest.h>

#include <llvh/Support/FileSystem.h>
#include <llvh/Support/MemoryBuffer.h>
#include <llvh/Support/SHA1.h>

#include <iterator>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "tests/TestFunctions.h"

using ParamType = std::tuple<
    const char *,
    std::function<const char *()>,
    std::function<const char *()>>;

#define TEST_FUNC_MAKE_TUPLE(name)               \
  ParamType{                                     \
      #name,                                     \
      facebook::hermes::synthtest::name##Trace,  \
      facebook::hermes::synthtest::name##Source, \
  },

namespace {

using namespace facebook::hermes;
using namespace hermes::vm;

class SynthBenchmarkTestFixture : public ::testing::TestWithParam<ParamType> {
 protected:
  SynthBenchmarkTestFixture()
      : testName_(std::get<0>(GetParam())),
        trace_(std::get<1>(GetParam())()),
        source_(std::get<2>(GetParam())()) {}
  const char *testName_;
  const char *trace_;
  const char *source_;

  /// All of the tests provide a trace.  We run each test in two modes
  /// -- specify property names as string IDs, or as PropNameIDs.
  /// The traces use the convention of creating fake
  /// "CreatePropNameRecords".  These are translated either into
  /// CreateStringRecords or CreatePropNameIDRecords, via string substitution.
  /// Similarly, property IDs are tagged with the type "propIDTag",
  /// which is also replaced via string substitution.  This method is then
  /// run on the translated trace.
  void canRunWithoutException(const std::string &trace) {
    tracing::TraceInterpreter::ExecuteOptions options;
    std::vector<std::unique_ptr<llvh::MemoryBuffer>> sources;
    sources.emplace_back(llvh::MemoryBuffer::getMemBuffer(source_));
    EXPECT_NO_THROW({
      tracing::TraceInterpreter::execFromMemoryBuffer(
          llvh::MemoryBuffer::getMemBuffer(trace.c_str()),
          std::move(sources),
          options,
          makeHermesRuntime);
    }) << "Failed on test: "
       << testName_;
  }

  /// Substitute \p to for \p from in \p str, modifying \p str.
  static void
  subst(const std::string &from, const std::string &to, std::string &str) {
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
      str.replace(pos, from.length(), to);
      pos += to.length();
    }
  }
};

TEST_P(SynthBenchmarkTestFixture, CanRunWithoutExceptionString) {
  // Convert CreatePropNameRecords to CreateStringRecords.
  std::string stringNameIDTrace{trace_};
  subst("CreatePropNameRecord", "CreateStringRecord", stringNameIDTrace);
  subst("\"propIDTag:", "\"string:", stringNameIDTrace);
  canRunWithoutException(stringNameIDTrace);
}

TEST_P(SynthBenchmarkTestFixture, CanRunWithoutExceptionPropNameID) {
  // Convert CreatePropNameRecords to CreatePropNameIDRecords.
  std::string propNameIDTrace{trace_};
  subst("CreatePropNameRecord", "CreatePropNameIDRecord", propNameIDTrace);
  subst("\"propIDTag:", "\"propNameID:", propNameIDTrace);
  canRunWithoutException(propNameIDTrace);
}

TEST(SynthBenchmark, RunMultipleSourceFiles) {
  tracing::TraceInterpreter::ExecuteOptions options;
  const char *const traceFmt = R"(
    {
      "globalObjID": 1,
      "trace": [
        {
          "type": "BeginExecJSRecord",
          "time": 0,
          "sourceHash": "%s"
        },
        {
          "type": "EndExecJSRecord",
          "retval": "string:1111",
          "time": 0
        },
        {
          "type": "BeginExecJSRecord",
          "time": 0,
          "sourceHash": "%s"
        },
        {
          "type": "EndExecJSRecord",
          "retval": "string:1112",
          "time": 0
        }
      ]
    })";
  std::string source1 = R"("hello";)";
  std::string source2 = R"("goodbye";)";
  std::string trace;
  {
    auto hash1 = llvh::SHA1::hash(
        llvh::makeArrayRef(
            reinterpret_cast<const uint8_t *>(source1.c_str()),
            source1.length()));
    auto hash2 = llvh::SHA1::hash(
        llvh::makeArrayRef(
            reinterpret_cast<const uint8_t *>(source2.c_str()),
            source2.length()));
    std::string hash1AsStr = ::hermes::hashAsString(hash1);
    std::string hash2AsStr = ::hermes::hashAsString(hash2);
    int numChars =
        snprintf(nullptr, 0, traceFmt, hash1AsStr.c_str(), hash2AsStr.c_str());
    char *buf = new char[numChars + 1];
    snprintf(
        buf, numChars + 1, traceFmt, hash1AsStr.c_str(), hash2AsStr.c_str());
    trace = std::string(buf);
    delete[] buf;
  }
  std::vector<std::unique_ptr<llvh::MemoryBuffer>> sources;
  sources.emplace_back(llvh::MemoryBuffer::getMemBuffer(source1));
  sources.emplace_back(llvh::MemoryBuffer::getMemBuffer(source2));
  EXPECT_NO_THROW({
    tracing::TraceInterpreter::execFromMemoryBuffer(
        llvh::MemoryBuffer::getMemBuffer(trace),
        std::move(sources),
        options,
        makeHermesRuntime);
  });
}

std::vector<ParamType> testGenerator() {
  return {FOREACH_TEST(TEST_FUNC_MAKE_TUPLE)};
}

INSTANTIATE_TEST_CASE_P(
    ExceptionTest,
    SynthBenchmarkTestFixture,
    ::testing::ValuesIn(testGenerator()));

} // namespace

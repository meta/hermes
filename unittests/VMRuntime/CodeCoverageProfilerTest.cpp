/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VMRuntimeTestHelpers.h"
#include "gtest/gtest.h"

#include "hermes/VM/Profiler/CodeCoverageProfiler.h"

#include "llvh/ADT/StringRef.h"
#include "llvh/Support/Compiler.h"
#include "llvh/Support/raw_ostream.h"

#include <algorithm>
#include <future>
#include <unordered_map>

using namespace hermes::vm;

namespace hermes {
namespace unittest {
namespace CodeCoverageTest {

class CodeCoverageProfilerTest : public RuntimeTestFixture {
 public:
  CodeCoverageProfilerTest() {
    CodeCoverageProfiler::enableGlobal();
  }
  ~CodeCoverageProfilerTest() {
    CodeCoverageProfiler::disableGlobal();
  }

 protected:
  static CodeCoverageProfiler::FuncInfo getFuncInfo(
      Runtime &runtime,
      Handle<JSFunction> func) {
    auto bcProvider = func->getCodeBlock()->getRuntimeModule()->getBytecode();
    auto functionId = func->getCodeBlock()->getFunctionID();
    auto debugInfo = bcProvider->getDebugInfo();
    auto debugOffsets = bcProvider->getDebugOffsets(functionId);
    if (debugInfo && debugOffsets &&
        debugOffsets->sourceLocations != hbc::DebugOffsets::NO_OFFSET) {
      if (auto pos = debugInfo->getLocationForAddress(
              debugOffsets->sourceLocations, 0 /* opcodeOffset */)) {
        auto file = debugInfo->getUTF8FilenameByID(pos->filenameId);
        auto line = pos->line - 1; // Normalised to zero-based
        auto column = pos->column - 1; // Normalised to zero-based
        return {line, column, file};
      }
    }
    const uint32_t segmentID = bcProvider->getSegmentID();
    const uint32_t funcVirtualOffset =
        bcProvider->getVirtualOffsetForFunction(functionId);
    const std::string sourceURL = func->getRuntimeModule()->getSourceURL();
    return {segmentID, funcVirtualOffset, sourceURL};
  }

  // Check if the function is executed or not.
  static bool isFuncExecuted(
      Runtime &runtime,
      const std::vector<CodeCoverageProfiler::FuncInfo> &executedFuncInfos,
      Handle<JSFunction> checkFunc) {
    CodeCoverageProfiler::FuncInfo checkFuncInfo =
        getFuncInfo(runtime, checkFunc);
    return std::find(
               executedFuncInfos.begin(),
               executedFuncInfos.end(),
               checkFuncInfo) != executedFuncInfos.end();
  }

  // Whether \p func1 and \p func2 have different FuncInfo
  // or not.
  static ::testing::AssertionResult hasDifferentInfo(
      Runtime &runtime,
      Handle<JSFunction> func1,
      Handle<JSFunction> func2) {
    if (getFuncInfo(runtime, func1) == getFuncInfo(runtime, func2)) {
      return ::testing::AssertionFailure()
          << "func1 and func2 has same func info.";
    } else {
      return ::testing::AssertionSuccess();
    }
  }
};

TEST_F(CodeCoverageProfilerTest, BasicFunctionUsedUnused) {
  struct : public Locals {
    PinnedValue<JSArray> funcArr;
    PinnedValue<JSFunction> funcUsed;
    PinnedValue<JSFunction> funcUnused;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  hbc::CompileFlags flags;
  flags.lazy = false;
  CallResult<HermesValue> res = runtime.run(
      "function used() {}; function unused() {}; used(); [used, unused];",
      "file:///fake.js",
      flags);
  EXPECT_FALSE(isException(res));

  std::unordered_map<std::string, std::vector<CodeCoverageProfiler::FuncInfo>>
      executedFuncInfos = CodeCoverageProfiler::getExecutedFunctions();
  std::vector<CodeCoverageProfiler::FuncInfo> testRuntimeExecutedFuncInfos =
      executedFuncInfos.find(runtime.getHeap().getName())->second;

  lv.funcArr.castAndSetHermesValue<JSArray>(*res);
  Handle<JSArray> funcArr = lv.funcArr;
  lv.funcUsed = vmcast<JSFunction>(funcArr->at(runtime, 0).getObject(runtime));
  Handle<JSFunction> funcUsed = lv.funcUsed;
  lv.funcUnused =
      vmcast<JSFunction>(funcArr->at(runtime, 1).getObject(runtime));
  Handle<JSFunction> funcUnused = lv.funcUnused;

  // Used and unused functions should have different info.
  EXPECT_TRUE(hasDifferentInfo(runtime, funcUsed, funcUnused));

  // Global + used.
  EXPECT_EQ(testRuntimeExecutedFuncInfos.size(), 2);
  EXPECT_TRUE(isFuncExecuted(runtime, testRuntimeExecutedFuncInfos, funcUsed));
  EXPECT_FALSE(
      isFuncExecuted(runtime, testRuntimeExecutedFuncInfos, funcUnused));
}

// Right now, this just tests that we can simultaneously run two code coverage
// profilers.
// Disabled for Apple ASAN builds because it causes a native stack overflow
// as a result of the large minimum stack gap.
// Consider enabling it again with larger stack for std::async.
#if !(LLVM_ADDRESS_SANITIZER_BUILD && defined(__APPLE__))
TEST_F(CodeCoverageProfilerTest, BasicFunctionUsedUnusedTwoRuntimes) {
  auto runtime2 = newRuntime();
  std::vector<Runtime *> runtimes = {&runtime, runtime2.get()};

  std::vector<std::future<PinnedHermesValue>> resFuts;

  for (auto *rt : runtimes) {
    resFuts.push_back(
        std::async(std::launch::async, [this, rt]() -> PinnedHermesValue {
          hbc::CompileFlags flags;
          flags.lazy = false;
          CallResult<HermesValue> res = rt->run(
              "function used() {}; function unused() {}; used(); [used, unused];",
              "file:///fake.js",
              flags);
          EXPECT_FALSE(isException(res));
          return *res;
        }));
  }

  for (size_t i = 0; i < runtimes.size(); i++) {
    Runtime &rt = *runtimes[i];
    // Wait for the thread using rt to finish before touching rt's Locals.
    HermesValue res = resFuts[i].get();
    struct : public Locals {
      PinnedValue<JSArray> funcArr;
      PinnedValue<JSFunction> funcUsed;
      PinnedValue<JSFunction> funcUnused;
    } lv;
    LocalsRAII lraii_lv(rt, &lv);
    lv.funcArr.castAndSetHermesValue<JSArray>(res);

    std::vector<CodeCoverageProfiler::FuncInfo> executedFuncInfos =
        rt.getCodeCoverageProfiler().getExecutedFunctionsLocal();

    Handle<JSArray> funcArr = lv.funcArr;
    lv.funcUsed = vmcast<JSFunction>(funcArr->at(rt, 0).getObject(rt));
    Handle<JSFunction> funcUsed = lv.funcUsed;
    lv.funcUnused = vmcast<JSFunction>(funcArr->at(rt, 1).getObject(rt));
    Handle<JSFunction> funcUnused = lv.funcUnused;

    // Used and unused functions should have different info.
    EXPECT_TRUE(hasDifferentInfo(rt, funcUsed, funcUnused));

    // Global + used.
    EXPECT_EQ(executedFuncInfos.size(), 2);
    EXPECT_TRUE(isFuncExecuted(rt, executedFuncInfos, funcUsed));
    EXPECT_FALSE(isFuncExecuted(rt, executedFuncInfos, funcUnused));
  }
}
#endif

TEST_F(CodeCoverageProfilerTest, FunctionsFromMultipleModules) {
  struct : public Locals {
    PinnedValue<JSFunction> funcFoo;
    PinnedValue<JSArray> funcArr;
    PinnedValue<JSFunction> funcBar;
    PinnedValue<JSFunction> funcUnused;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  hbc::CompileFlags flags;
  flags.lazy = false;
  CallResult<HermesValue> res1 =
      runtime.run("function foo() {}; foo(); foo;", "file:///fake1.js", flags);
  EXPECT_FALSE(isException(res1));
  lv.funcFoo.castAndSetHermesValue<JSFunction>(*res1);
  Handle<JSFunction> funcFoo = lv.funcFoo;

  CallResult<HermesValue> res2 = runtime.run(
      "\n  function bar() {}; function bar() {}; function unused() {}; bar(); [bar, unused];",
      "file:///fake2.js",
      flags);
  EXPECT_FALSE(isException(res2));

  std::unordered_map<std::string, std::vector<CodeCoverageProfiler::FuncInfo>>
      executedFuncInfos = CodeCoverageProfiler::getExecutedFunctions();
  std::vector<CodeCoverageProfiler::FuncInfo> testRuntimeExecutedFuncInfos =
      executedFuncInfos.find(runtime.getHeap().getName())->second;

  lv.funcArr.castAndSetHermesValue<JSArray>(*res2);
  Handle<JSArray> funcArr = lv.funcArr;
  lv.funcBar = vmcast<JSFunction>(funcArr->at(runtime, 0).getObject(runtime));
  Handle<JSFunction> funcBar = lv.funcBar;
  lv.funcUnused =
      vmcast<JSFunction>(funcArr->at(runtime, 1).getObject(runtime));
  Handle<JSFunction> funcUnused = lv.funcUnused;

  // Used and unused functions should have different info.
  EXPECT_TRUE(hasDifferentInfo(runtime, funcFoo, funcUnused));
  EXPECT_TRUE(hasDifferentInfo(runtime, funcBar, funcUnused));

  EXPECT_EQ(testRuntimeExecutedFuncInfos.size(), 4);
  EXPECT_TRUE(isFuncExecuted(runtime, testRuntimeExecutedFuncInfos, funcFoo));
  EXPECT_TRUE(isFuncExecuted(runtime, testRuntimeExecutedFuncInfos, funcBar));
  EXPECT_FALSE(
      isFuncExecuted(runtime, testRuntimeExecutedFuncInfos, funcUnused));
}

TEST_F(CodeCoverageProfilerTest, FunctionsFromMultipleDomains) {
  struct : public Locals {
    PinnedValue<JSArray> funcArr;
    PinnedValue<JSFunction> funcUsed1;
    PinnedValue<JSFunction> funcUsed2;
    PinnedValue<JSFunction> funcUnused;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  hbc::CompileFlags flags;
  flags.lazy = false;
  CallResult<HermesValue> res = runtime.run(
      "var eval1 = eval('const a = 1; function used1() {}; used1(); used1; //# sourceURL=foo1.js'); "
      "var eval2 = eval('const b = 1; function unused() {}; function used2() {}; used2(); [used2, unused] //# sourceURL=foo2.js');"
      "[eval1, eval2[0], eval2[1]];",
      "file:///fake.js",
      flags);
  EXPECT_FALSE(isException(res));

  std::unordered_map<std::string, std::vector<CodeCoverageProfiler::FuncInfo>>
      executedFuncInfos = CodeCoverageProfiler::getExecutedFunctions();
  std::vector<CodeCoverageProfiler::FuncInfo> testRuntimeExecutedFuncInfos =
      executedFuncInfos.find(runtime.getHeap().getName())->second;

  lv.funcArr.castAndSetHermesValue<JSArray>(*res);
  Handle<JSArray> funcArr = lv.funcArr;
  lv.funcUsed1 = vmcast<JSFunction>(funcArr->at(runtime, 0).getObject(runtime));
  Handle<JSFunction> funcUsed1 = lv.funcUsed1;
  lv.funcUsed2 = vmcast<JSFunction>(funcArr->at(runtime, 1).getObject(runtime));
  Handle<JSFunction> funcUsed2 = lv.funcUsed2;
  lv.funcUnused =
      vmcast<JSFunction>(funcArr->at(runtime, 2).getObject(runtime));
  Handle<JSFunction> funcUnused = lv.funcUnused;

  // Used and unused functions should have different info.
  EXPECT_TRUE(hasDifferentInfo(runtime, funcUsed1, funcUnused));
  EXPECT_TRUE(hasDifferentInfo(runtime, funcUsed2, funcUnused));

  // Global + two eval() code + used1 + used2.
  EXPECT_EQ(testRuntimeExecutedFuncInfos.size(), 5);
  EXPECT_TRUE(isFuncExecuted(runtime, testRuntimeExecutedFuncInfos, funcUsed1));
  EXPECT_TRUE(isFuncExecuted(runtime, testRuntimeExecutedFuncInfos, funcUsed2));
  EXPECT_FALSE(
      isFuncExecuted(runtime, testRuntimeExecutedFuncInfos, funcUnused));
}

TEST_F(CodeCoverageProfilerTest, LazyFunction) {
  struct : public Locals {
    PinnedValue<JSFunction> funcFoo;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  hbc::CompileFlags flags;
  flags.lazy = true;
  // Force lazy compilation.
  flags.preemptiveFileCompilationThreshold = 0;
  flags.preemptiveFunctionCompilationThreshold = 0;
  CallResult<HermesValue> res = runtime.run(
      "var foo; var bar = function() { foo = function() { return 5; }; return foo;}; bar()(); foo;",
      "file:///fake.js",
      flags);
  EXPECT_FALSE(isException(res));

  std::unordered_map<std::string, std::vector<CodeCoverageProfiler::FuncInfo>>
      executedFuncInfos = CodeCoverageProfiler::getExecutedFunctions();
  std::vector<CodeCoverageProfiler::FuncInfo> testRuntimeExecutedFuncInfos =
      executedFuncInfos.find(runtime.getHeap().getName())->second;

  lv.funcFoo.castAndSetHermesValue<JSFunction>(*res);
  Handle<JSFunction> funcFoo = lv.funcFoo;
  // Global + bar + foo.
  EXPECT_EQ(testRuntimeExecutedFuncInfos.size(), 3);
  EXPECT_TRUE(isFuncExecuted(runtime, testRuntimeExecutedFuncInfos, funcFoo));
}

} // namespace CodeCoverageTest
} // namespace unittest
} // namespace hermes

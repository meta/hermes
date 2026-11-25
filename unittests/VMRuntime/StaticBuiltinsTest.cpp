/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/BCGen/HBC/BCProviderFromSrc.h"
#include "hermes/Support/MemoryBuffer.h"
#include "hermes/VM/Runtime.h"

#include "VMRuntimeTestHelpers.h"

#include "gtest/gtest.h"

#include <string>

using namespace hermes;
using namespace hermes::vm;
using namespace hermes::hbc;

namespace {

/// Assert that obj.prop is normal
#define EXPECT_PROPERTY_NORMAL(obj, prop)                                   \
  {                                                                         \
    NamedPropertyDescriptor desc;                                           \
    ASSERT_TRUE(                                                            \
        JSObject::getNamedDescriptorPredefined(obj, runtime, prop, desc) != \
        nullptr);                                                           \
    EXPECT_TRUE(desc.flags.writable);                                       \
    EXPECT_TRUE(desc.flags.configurable);                                   \
    EXPECT_FALSE(desc.flags.staticBuiltin);                                 \
  }

/// Assert that obj.prop is frozen and the static builtin flag is marked.
#define EXPECT_PROPERTY_FROZEN_AND_MARKED_AS_STATIC(obj, prop)              \
  {                                                                         \
    NamedPropertyDescriptor desc;                                           \
    ASSERT_TRUE(                                                            \
        JSObject::getNamedDescriptorPredefined(obj, runtime, prop, desc) != \
        nullptr);                                                           \
    EXPECT_FALSE(desc.flags.writable);                                      \
    EXPECT_FALSE(desc.flags.configurable);                                  \
    EXPECT_TRUE(desc.flags.staticBuiltin);                                  \
  }

/// Verify builtin objects and the builtin methods on those objects
/// are frozen and marked as static builtins.
static void verifyAllBuiltinsFrozen(Runtime &runtime) {
  GCScope gcScope{runtime};
  struct : public Locals {
    PinnedValue<JSObject> objHandleSlot;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  auto global = runtime.getGlobal();
  Predefined::getSymbolID(Predefined::isArray);
#define NORMAL_OBJECT(object) \
  {EXPECT_PROPERTY_NORMAL(global, Predefined::object)}
#define BUILTIN_OBJECT(object) \
  {EXPECT_PROPERTY_FROZEN_AND_MARKED_AS_STATIC(global, Predefined::object)}

  MutableHandle<JSObject> objHandle{lv.objHandleSlot};

#define NORMAL_METHOD(object, method)                            \
  {                                                              \
    auto objectID = Predefined::getSymbolID(Predefined::object); \
    auto cr = JSObject::getNamed_RJS(global, runtime, objectID); \
    ASSERT_NE(cr, ExecutionStatus::EXCEPTION);                   \
    objHandle = vmcast<JSObject>(cr->get());                     \
    EXPECT_PROPERTY_NORMAL(objHandle, Predefined::method);       \
  }
#define BUILTIN_METHOD(object, method)                           \
  {                                                              \
    auto objectID = Predefined::getSymbolID(Predefined::object); \
    auto cr = JSObject::getNamed_RJS(global, runtime, objectID); \
    ASSERT_NE(cr, ExecutionStatus::EXCEPTION);                   \
    objHandle = vmcast<JSObject>(cr->get());                     \
    EXPECT_PROPERTY_FROZEN_AND_MARKED_AS_STATIC(                 \
        objHandle, Predefined::method);                          \
  }
#include "hermes/FrontEndDefs/Builtins.def"
}

class StaticBuiltinsTest : public RuntimeTestFixture {
 public:
  StaticBuiltinsTest() {}
};

TEST_F(StaticBuiltinsTest, FreezeBuiltins) {
  std::string testCode = R"(
    function assert(condition) {
      if (!condition) {
        throw Error();
      }
    }
    assert(Array.isArray([1, 2]));
    assert(Math.abs(-1) === 1);
    assert(JSON.stringify(Object.keys({a:1, b: 2})) === '["a","b"]');
    assert(String.fromCharCode(65, 66, 67) === "ABC");
  )";

  // run normal bytecode first
  CompileFlags flagsNone;
  flagsNone.staticBuiltins = false;
  EXPECT_EQ(
      runtime.run(testCode, "source/url", flagsNone),
      ExecutionStatus::RETURNED);
  EXPECT_FALSE(runtime.builtinsAreFrozen());

  // run bytecode with static builtins
  CompileFlags flagsBuiltin;
  flagsBuiltin.staticBuiltins = true;
  EXPECT_EQ(
      runtime.run(testCode, "source/url", flagsBuiltin),
      ExecutionStatus::RETURNED);
  EXPECT_TRUE(runtime.builtinsAreFrozen());
  verifyAllBuiltinsFrozen(runtime);

  // run normal bytecode again
  EXPECT_EQ(
      runtime.run(testCode, "source/url", flagsNone),
      ExecutionStatus::RETURNED);
  EXPECT_TRUE(runtime.builtinsAreFrozen());

  // run bytecode with builtins again
  EXPECT_EQ(
      runtime.run(testCode, "source/url", flagsNone),
      ExecutionStatus::RETURNED);
  EXPECT_TRUE(runtime.builtinsAreFrozen());

  verifyAllBuiltinsFrozen(runtime);
}

TEST_F(StaticBuiltinsTest, BuiltinsOverridden) {
  // run bytecode that overrides a builtin method
  std::string codeChangeBuiltin = R"(
    function assert(condition) {
      if (!condition) {
        throw Error();
      }
    }
    Array.isArray = true;
    assert(Array.isArray);
  )";
  CompileFlags flagsNone;
  flagsNone.staticBuiltins = false;
  EXPECT_EQ(
      runtime.run(codeChangeBuiltin, "source/url", flagsNone),
      ExecutionStatus::RETURNED);
  EXPECT_FALSE(runtime.builtinsAreFrozen());

  // run bytecode compiled with static builtins enabled
  std::string codeStaticBuiltin = R"(
    print("hello world");
  )";
  CompileFlags flagsBuiltin;
  flagsBuiltin.staticBuiltins = true;
  EXPECT_EQ(
      runtime.run(codeStaticBuiltin, "source/url", flagsBuiltin),
      ExecutionStatus::EXCEPTION);
}

TEST_F(StaticBuiltinsTest, AttemptToOverrideBuiltins) {
  // Run bytecode compiled with static builtins enabled but attempt to override
  // a builtin method at the same time. We should get an exception.
  std::string codeStaticBuiltin = R"(
    Math.sin = false;
  )";
  CompileFlags flagsBuiltin;
  flagsBuiltin.staticBuiltins = true;
  EXPECT_EQ(
      runtime.run(codeStaticBuiltin, "source/url", flagsBuiltin),
      ExecutionStatus::EXCEPTION);
}

TEST_F(StaticBuiltinsTest, AttemptToOverrideBuiltins2) {
  // Run bytecode compiled with static builtins enabled to freeze the builtins.
  std::string codeStaticBuiltin = R"(
    print('hello world');
  )";
  CompileFlags flagsBuiltin;
  flagsBuiltin.staticBuiltins = true;
  EXPECT_EQ(
      runtime.run(codeStaticBuiltin, "source/url", flagsBuiltin),
      ExecutionStatus::RETURNED);

  // Run bytecode that attempts to override a builtin method. We should get an
  // exception.
  std::string codeOverrideBuiltin = R"(
    Math.sin = false;
  )";
  CompileFlags flagsNone;
  flagsNone.staticBuiltins = false;
  EXPECT_EQ(
      runtime.run(codeOverrideBuiltin, "source/url", flagsNone),
      ExecutionStatus::EXCEPTION);
}

TEST_F(StaticBuiltinsTest, UseStaticBuiltinDirective) {
  std::string codeStaticBuiltin = R"(
    'use static builtin';
    Array.isArray = 1;
  )";
  CompileFlags flagsAutoBuiltin;
  EXPECT_EQ(
      runtime.run(codeStaticBuiltin, "source/url", flagsAutoBuiltin),
      ExecutionStatus::EXCEPTION);
}

TEST_F(StaticBuiltinsTest, ForceNoBuiltinFlag) {
  std::string codeStaticBuiltin = R"(
    'use static builtin';
    Array.isArray = 1;
  )";
  CompileFlags flagsForceNoBuiltin;
  flagsForceNoBuiltin.staticBuiltins = false;
  EXPECT_EQ(
      runtime.run(codeStaticBuiltin, "source/url", flagsForceNoBuiltin),
      ExecutionStatus::RETURNED);
}

TEST_F(StaticBuiltinsTest, UseStaticBuiltinDirectiveLazyCompilation) {
  std::string codeStaticBuiltin = R"(
    Array.isArray = 1;
    function func() {
      'use static builtin';
      return;
    }
  )";
  CompileFlags flagsAutoBuiltinLazy;
  flagsAutoBuiltinLazy.lazy = true;
  flagsAutoBuiltinLazy.preemptiveFunctionCompilationThreshold = 0;
  flagsAutoBuiltinLazy.preemptiveFileCompilationThreshold = 0;
  EXPECT_EQ(
      runtime.run(codeStaticBuiltin, "source/url", flagsAutoBuiltinLazy),
      ExecutionStatus::EXCEPTION);
}

TEST_F(StaticBuiltinsTest, ForceNoBuiltinFlagLazyCompilation) {
  std::string codeStaticBuiltin = R"(
    Array.isArray = 1;
    function func() {
      'use static builtin';
      return;
    }
  )";
  CompileFlags flagsForceNoBuiltin;
  flagsForceNoBuiltin.staticBuiltins = false;
  flagsForceNoBuiltin.lazy = true;
  flagsForceNoBuiltin.preemptiveFunctionCompilationThreshold = 0;
  flagsForceNoBuiltin.preemptiveFileCompilationThreshold = 0;
  EXPECT_EQ(
      runtime.run(codeStaticBuiltin, "source/url", flagsForceNoBuiltin),
      ExecutionStatus::RETURNED);
}

} // namespace

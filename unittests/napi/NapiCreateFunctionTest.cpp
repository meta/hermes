/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NapiTestFixture.h"

#include "hermes/VM/Callable.h"
#include "hermes/VM/HandleRootOwner.h"
#include "hermes/VM/HermesValue.h"

namespace {

using hermes::napi::NapiTestFixture;
using namespace hermes::vm;

/// Helper to open a handle scope and return the scope handle, asserting
/// success.
static napi_handle_scope openScope(napi_env env) {
  napi_handle_scope scope = nullptr;
  EXPECT_EQ(napi_ok, napi_open_handle_scope(env, &scope));
  EXPECT_NE(nullptr, scope);
  return scope;
}

/// Helper to close a handle scope, asserting success.
static void closeScope(napi_env env, napi_handle_scope scope) {
  EXPECT_EQ(napi_ok, napi_close_handle_scope(env, scope));
}

/// Helper to call a napi_value function via Hermes VM APIs.
/// Calls the function with `this` = undefined and the given arguments.
static CallResult<PseudoHandle<>> callFunction(
    Runtime &runtime,
    napi_value funcVal,
    Handle<> thisArg,
    llvh::ArrayRef<HermesValue> args) {
  auto *phv = reinterpret_cast<PinnedHermesValue *>(funcVal);
  auto funcHandle = Handle<Callable>::vmcast(phv);

  switch (args.size()) {
    case 0:
      return Callable::executeCall0(funcHandle, runtime, thisArg);
    case 1:
      return Callable::executeCall1(funcHandle, runtime, thisArg, args[0]);
    case 2:
      return Callable::executeCall2(
          funcHandle, runtime, thisArg, args[0], args[1]);
    case 3:
      return Callable::executeCall3(
          funcHandle, runtime, thisArg, args[0], args[1], args[2]);
    default:
      llvm_unreachable("too many args for test helper");
  }
}

//===========================================================================
// napi_create_function tests
//===========================================================================

class NapiCreateFunctionTest : public NapiTestFixture {};

TEST_F(NapiCreateFunctionTest, NullEnv) {
  napi_value result = nullptr;
  napi_callback cb = [](napi_env, napi_callback_info) -> napi_value {
    return nullptr;
  };
  EXPECT_EQ(
      napi_invalid_arg,
      napi_create_function(nullptr, nullptr, 0, cb, nullptr, &result));
}

TEST_F(NapiCreateFunctionTest, NullResult) {
  napi_callback cb = [](napi_env, napi_callback_info) -> napi_value {
    return nullptr;
  };
  EXPECT_EQ(
      napi_invalid_arg,
      napi_create_function(env_, nullptr, 0, cb, nullptr, nullptr));
}

TEST_F(NapiCreateFunctionTest, NullCallback) {
  auto scope = openScope(env_);
  napi_value result = nullptr;
  EXPECT_EQ(
      napi_invalid_arg,
      napi_create_function(env_, nullptr, 0, nullptr, nullptr, &result));
  closeScope(env_, scope);
}

TEST_F(NapiCreateFunctionTest, CreateAnonymous) {
  auto scope = openScope(env_);

  napi_callback cb = [](napi_env, napi_callback_info) -> napi_value {
    return nullptr;
  };

  napi_value func = nullptr;
  EXPECT_EQ(
      napi_ok, napi_create_function(env_, nullptr, 0, cb, nullptr, &func));
  ASSERT_NE(nullptr, func);

  // Verify it's a function.
  napi_valuetype type;
  EXPECT_EQ(napi_ok, napi_typeof(env_, func, &type));
  EXPECT_EQ(napi_function, type);

  closeScope(env_, scope);
}

TEST_F(NapiCreateFunctionTest, CreateNamed) {
  auto scope = openScope(env_);

  napi_callback cb = [](napi_env, napi_callback_info) -> napi_value {
    return nullptr;
  };

  napi_value func = nullptr;
  EXPECT_EQ(
      napi_ok,
      napi_create_function(
          env_, "myFunc", NAPI_AUTO_LENGTH, cb, nullptr, &func));
  ASSERT_NE(nullptr, func);

  // Verify it's a function.
  napi_valuetype type;
  EXPECT_EQ(napi_ok, napi_typeof(env_, func, &type));
  EXPECT_EQ(napi_function, type);

  // Verify the function name via the "name" property.
  napi_value nameKey = nullptr;
  EXPECT_EQ(
      napi_ok,
      napi_create_string_utf8(env_, "name", NAPI_AUTO_LENGTH, &nameKey));

  napi_value nameVal = nullptr;
  EXPECT_EQ(napi_ok, napi_get_property(env_, func, nameKey, &nameVal));

  char buf[32] = {};
  size_t len = 0;
  EXPECT_EQ(
      napi_ok,
      napi_get_value_string_utf8(env_, nameVal, buf, sizeof(buf), &len));
  EXPECT_EQ(std::string("myFunc"), std::string(buf, len));

  closeScope(env_, scope);
}

TEST_F(NapiCreateFunctionTest, CreateWithExplicitLength) {
  auto scope = openScope(env_);

  napi_callback cb = [](napi_env, napi_callback_info) -> napi_value {
    return nullptr;
  };

  // Name is "hello" but length is 3, so only "hel" is used.
  napi_value func = nullptr;
  EXPECT_EQ(
      napi_ok, napi_create_function(env_, "hello", 3, cb, nullptr, &func));
  ASSERT_NE(nullptr, func);

  // Verify the function name is "hel".
  napi_value nameKey = nullptr;
  EXPECT_EQ(
      napi_ok,
      napi_create_string_utf8(env_, "name", NAPI_AUTO_LENGTH, &nameKey));

  napi_value nameVal = nullptr;
  EXPECT_EQ(napi_ok, napi_get_property(env_, func, nameKey, &nameVal));

  char buf[32] = {};
  size_t len = 0;
  EXPECT_EQ(
      napi_ok,
      napi_get_value_string_utf8(env_, nameVal, buf, sizeof(buf), &len));
  EXPECT_EQ(std::string("hel"), std::string(buf, len));

  closeScope(env_, scope);
}

TEST_F(NapiCreateFunctionTest, CallReturnsUndefined) {
  auto scope = openScope(env_);

  // A callback that returns null (meaning "return undefined").
  napi_callback cb = [](napi_env, napi_callback_info) -> napi_value {
    return nullptr;
  };

  napi_value func = nullptr;
  EXPECT_EQ(
      napi_ok, napi_create_function(env_, nullptr, 0, cb, nullptr, &func));

  // Call it via Hermes VM APIs.
  auto res =
      callFunction(env_->runtime, func, Runtime::getUndefinedValue(), {});
  ASSERT_NE(ExecutionStatus::EXCEPTION, res.getStatus());
  EXPECT_TRUE(res->get().isUndefined());

  closeScope(env_, scope);
}

TEST_F(NapiCreateFunctionTest, CallReturnsNumber) {
  auto scope = openScope(env_);

  // A callback that returns the number 42.
  napi_callback cb = [](napi_env env, napi_callback_info) -> napi_value {
    napi_value result = nullptr;
    napi_create_double(env, 42.0, &result);
    return result;
  };

  napi_value func = nullptr;
  EXPECT_EQ(
      napi_ok, napi_create_function(env_, nullptr, 0, cb, nullptr, &func));

  // Call it.
  auto res =
      callFunction(env_->runtime, func, Runtime::getUndefinedValue(), {});
  ASSERT_NE(ExecutionStatus::EXCEPTION, res.getStatus());
  EXPECT_TRUE(res->get().isNumber());
  EXPECT_EQ(42.0, res->get().getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiCreateFunctionTest, CallWithArgsAddsNumbers) {
  auto scope = openScope(env_);

  // A callback that adds two numbers.
  napi_callback cb = [](napi_env env, napi_callback_info info) -> napi_value {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    double a = 0, b = 0;
    napi_get_value_double(env, argv[0], &a);
    napi_get_value_double(env, argv[1], &b);

    napi_value result = nullptr;
    napi_create_double(env, a + b, &result);
    return result;
  };

  napi_value func = nullptr;
  EXPECT_EQ(
      napi_ok,
      napi_create_function(env_, "add", NAPI_AUTO_LENGTH, cb, nullptr, &func));

  // Call it with args 10 and 32.
  HermesValue args[] = {
      HermesValue::encodeTrustedNumberValue(10),
      HermesValue::encodeTrustedNumberValue(32)};
  auto res =
      callFunction(env_->runtime, func, Runtime::getUndefinedValue(), args);
  ASSERT_NE(ExecutionStatus::EXCEPTION, res.getStatus());
  EXPECT_TRUE(res->get().isNumber());
  EXPECT_EQ(42.0, res->get().getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiCreateFunctionTest, CallbackReceivesThisArg) {
  auto scope = openScope(env_);

  // A callback that returns 'this'.
  napi_callback cb = [](napi_env env, napi_callback_info info) -> napi_value {
    napi_value this_arg = nullptr;
    napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr);
    return this_arg;
  };

  napi_value func = nullptr;
  EXPECT_EQ(
      napi_ok, napi_create_function(env_, nullptr, 0, cb, nullptr, &func));

  // Call it with 'this' = 99.
  struct : public Locals {
    PinnedValue<> thisVal;
  } lv;
  LocalsRAII lraii(env_->runtime, &lv);
  lv.thisVal = HermesValue::encodeTrustedNumberValue(99);
  auto res = callFunction(env_->runtime, func, lv.thisVal, {});
  ASSERT_NE(ExecutionStatus::EXCEPTION, res.getStatus());
  EXPECT_TRUE(res->get().isNumber());
  EXPECT_EQ(99.0, res->get().getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiCreateFunctionTest, CallbackReceivesData) {
  auto scope = openScope(env_);

  int userData = 123;

  // A callback that checks the data pointer and returns its value.
  napi_callback cb = [](napi_env env, napi_callback_info info) -> napi_value {
    void *data = nullptr;
    napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
    int val = *static_cast<int *>(data);

    napi_value result = nullptr;
    napi_create_double(env, val, &result);
    return result;
  };

  napi_value func = nullptr;
  EXPECT_EQ(
      napi_ok, napi_create_function(env_, nullptr, 0, cb, &userData, &func));

  // Call it.
  auto res =
      callFunction(env_->runtime, func, Runtime::getUndefinedValue(), {});
  ASSERT_NE(ExecutionStatus::EXCEPTION, res.getStatus());
  EXPECT_TRUE(res->get().isNumber());
  EXPECT_EQ(123.0, res->get().getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiCreateFunctionTest, CallbackThrowsException) {
  auto scope = openScope(env_);

  // A callback that throws a TypeError.
  napi_callback cb = [](napi_env env, napi_callback_info) -> napi_value {
    napi_throw_type_error(env, nullptr, "test error");
    return nullptr;
  };

  napi_value func = nullptr;
  EXPECT_EQ(
      napi_ok, napi_create_function(env_, nullptr, 0, cb, nullptr, &func));

  // Call it — should get an exception.
  auto res =
      callFunction(env_->runtime, func, Runtime::getUndefinedValue(), {});
  EXPECT_EQ(ExecutionStatus::EXCEPTION, res.getStatus());

  // Clear the runtime exception so TearDown doesn't complain.
  env_->runtime.clearThrownValue();

  closeScope(env_, scope);
}

TEST_F(NapiCreateFunctionTest, PendingExceptionBlocksCreate) {
  auto scope = openScope(env_);

  // Set a pending exception.
  napi_value errMsg = nullptr;
  EXPECT_EQ(
      napi_ok,
      napi_create_string_utf8(env_, "test", NAPI_AUTO_LENGTH, &errMsg));
  napi_throw(env_, errMsg);

  // napi_create_function should fail because of the pending exception.
  napi_callback cb = [](napi_env, napi_callback_info) -> napi_value {
    return nullptr;
  };
  napi_value func = nullptr;
  EXPECT_EQ(
      napi_pending_exception,
      napi_create_function(env_, nullptr, 0, cb, nullptr, &func));

  // Clear the exception.
  napi_value exc = nullptr;
  napi_get_and_clear_last_exception(env_, &exc);

  closeScope(env_, scope);
}

TEST_F(NapiCreateFunctionTest, CallbackReturnsString) {
  auto scope = openScope(env_);

  // A callback that returns a string.
  napi_callback cb = [](napi_env env, napi_callback_info) -> napi_value {
    napi_value result = nullptr;
    napi_create_string_utf8(env, "hello", NAPI_AUTO_LENGTH, &result);
    return result;
  };

  napi_value func = nullptr;
  EXPECT_EQ(
      napi_ok, napi_create_function(env_, nullptr, 0, cb, nullptr, &func));

  // Call it.
  auto res =
      callFunction(env_->runtime, func, Runtime::getUndefinedValue(), {});
  ASSERT_NE(ExecutionStatus::EXCEPTION, res.getStatus());
  EXPECT_TRUE(res->get().isString());

  closeScope(env_, scope);
}

TEST_F(NapiCreateFunctionTest, MultipleCreatedFunctions) {
  auto scope = openScope(env_);

  // Create two functions that return different values.
  napi_callback cb1 = [](napi_env env, napi_callback_info) -> napi_value {
    napi_value result = nullptr;
    napi_create_double(env, 1.0, &result);
    return result;
  };
  napi_callback cb2 = [](napi_env env, napi_callback_info) -> napi_value {
    napi_value result = nullptr;
    napi_create_double(env, 2.0, &result);
    return result;
  };

  napi_value func1 = nullptr, func2 = nullptr;
  EXPECT_EQ(
      napi_ok,
      napi_create_function(
          env_, "one", NAPI_AUTO_LENGTH, cb1, nullptr, &func1));
  EXPECT_EQ(
      napi_ok,
      napi_create_function(
          env_, "two", NAPI_AUTO_LENGTH, cb2, nullptr, &func2));

  // Call both and verify different results.
  {
    auto res1 =
        callFunction(env_->runtime, func1, Runtime::getUndefinedValue(), {});
    ASSERT_NE(ExecutionStatus::EXCEPTION, res1.getStatus());
    EXPECT_EQ(1.0, res1->get().getNumber());
  }

  {
    auto res2 =
        callFunction(env_->runtime, func2, Runtime::getUndefinedValue(), {});
    ASSERT_NE(ExecutionStatus::EXCEPTION, res2.getStatus());
    EXPECT_EQ(2.0, res2->get().getNumber());
  }

  closeScope(env_, scope);
}

TEST_F(NapiCreateFunctionTest, CallbackNoArgsPassed) {
  auto scope = openScope(env_);

  // A callback that requests 2 args but none are passed.
  // The missing args should be undefined.
  napi_callback cb = [](napi_env env, napi_callback_info info) -> napi_value {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    // argc should be set to 0 (actual count).
    // But argv[0] and argv[1] should be undefined.
    napi_valuetype t0, t1;
    napi_typeof(env, argv[0], &t0);
    napi_typeof(env, argv[1], &t1);

    // Return 1 if both are undefined, 0 otherwise.
    napi_value result = nullptr;
    napi_create_double(
        env,
        (t0 == napi_undefined && t1 == napi_undefined) ? 1.0 : 0.0,
        &result);
    return result;
  };

  napi_value func = nullptr;
  EXPECT_EQ(
      napi_ok, napi_create_function(env_, nullptr, 0, cb, nullptr, &func));

  // Call with no args.
  auto res =
      callFunction(env_->runtime, func, Runtime::getUndefinedValue(), {});
  ASSERT_NE(ExecutionStatus::EXCEPTION, res.getStatus());
  EXPECT_EQ(1.0, res->get().getNumber());

  closeScope(env_, scope);
}

} // namespace

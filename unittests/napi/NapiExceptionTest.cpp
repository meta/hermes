/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NapiTestFixture.h"

#include "hermes/Support/UTF8.h"
#include "hermes/VM/JSError.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/StringPrimitive.h"

namespace {

using hermes::napi::NapiTestFixture;
using hermes::vm::HermesValue;
using hermes::vm::JSError;
using hermes::vm::JSObject;
using hermes::vm::StringPrimitive;

//===========================================================================
// NAPI_PREAMBLE behavior
//===========================================================================

// A small test helper that uses NAPI_PREAMBLE to verify the macro works.
// This is NOT a real NAPI function — it's just a function that exercises
// the pending exception check.
static napi_status testPreambleFunction(napi_env env, int *out) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, out);
  *out = 42;
  return napi_clear_last_error(env);
}

TEST_F(NapiTestFixture, PreambleRejectsNullEnv) {
  int out = 0;
  EXPECT_EQ(napi_invalid_arg, testPreambleFunction(nullptr, &out));
  EXPECT_EQ(0, out); // Not modified.
}

TEST_F(NapiTestFixture, PreambleAllowsWhenNoException) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  int out = 0;
  EXPECT_EQ(napi_ok, testPreambleFunction(env_, &out));
  EXPECT_EQ(42, out);

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, PreambleRejectsWhenExceptionPending) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  // Manually set a pending exception.
  env_->hasPendingException = true;
  env_->pendingException = hermes::vm::HermesValue::encodeUndefinedValue();

  int out = 0;
  EXPECT_EQ(napi_pending_exception, testPreambleFunction(env_, &out));
  EXPECT_EQ(0, out); // Function body was not executed.

  // Clear the exception.
  env_->hasPendingException = false;

  // Now the function should succeed again.
  out = 0;
  EXPECT_EQ(napi_ok, testPreambleFunction(env_, &out));
  EXPECT_EQ(42, out);

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, PreambleSetsLastErrorOnPendingException) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  // Manually set a pending exception.
  env_->hasPendingException = true;
  env_->pendingException = hermes::vm::HermesValue::encodeUndefinedValue();

  int out = 0;
  EXPECT_EQ(napi_pending_exception, testPreambleFunction(env_, &out));

  // Verify last_error was set correctly.
  EXPECT_EQ(napi_pending_exception, env_->last_error.error_code);

  // Query error info.
  const napi_extended_error_info *info = nullptr;
  ASSERT_EQ(napi_ok, napi_get_last_error_info(env_, &info));
  ASSERT_NE(nullptr, info);
  EXPECT_EQ(napi_pending_exception, info->error_code);
  EXPECT_STREQ("An exception is pending", info->error_message);

  // Clean up.
  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

//===========================================================================
// Exception-safe functions should work even with pending exception
//===========================================================================

TEST_F(NapiTestFixture, ExceptionSafeGetVersion) {
  // napi_get_version should work even with a pending exception.
  env_->hasPendingException = true;
  env_->pendingException = hermes::vm::HermesValue::encodeUndefinedValue();

  uint32_t version = 0;
  EXPECT_EQ(napi_ok, napi_get_version(env_, &version));
  EXPECT_EQ(static_cast<uint32_t>(NAPI_VERSION), version);

  env_->hasPendingException = false;
}

TEST_F(NapiTestFixture, ExceptionSafeGetNodeVersion) {
  env_->hasPendingException = true;
  env_->pendingException = hermes::vm::HermesValue::encodeUndefinedValue();

  const napi_node_version *version = nullptr;
  EXPECT_EQ(napi_ok, napi_get_node_version(env_, &version));
  ASSERT_NE(nullptr, version);
  EXPECT_STREQ("hermes", version->release);

  env_->hasPendingException = false;
}

TEST_F(NapiTestFixture, ExceptionSafeGetLastErrorInfo) {
  env_->hasPendingException = true;
  env_->pendingException = hermes::vm::HermesValue::encodeUndefinedValue();

  const napi_extended_error_info *info = nullptr;
  EXPECT_EQ(napi_ok, napi_get_last_error_info(env_, &info));
  ASSERT_NE(nullptr, info);

  env_->hasPendingException = false;
}

TEST_F(NapiTestFixture, ExceptionSafeHandleScopes) {
  // Handle scope operations should work even with a pending exception.
  env_->hasPendingException = true;
  env_->pendingException = hermes::vm::HermesValue::encodeUndefinedValue();

  napi_handle_scope scope;
  EXPECT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));
  EXPECT_EQ(napi_ok, napi_close_handle_scope(env_, scope));

  env_->hasPendingException = false;
}

TEST_F(NapiTestFixture, ExceptionSafeEscapableHandleScopes) {
  napi_handle_scope outer;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &outer));

  env_->hasPendingException = true;
  env_->pendingException = hermes::vm::HermesValue::encodeUndefinedValue();

  napi_escapable_handle_scope escScope;
  EXPECT_EQ(napi_ok, napi_open_escapable_handle_scope(env_, &escScope));
  EXPECT_EQ(napi_ok, napi_close_escapable_handle_scope(env_, escScope));

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, outer));
}

TEST_F(NapiTestFixture, ExceptionSafeGetUndefined) {
  // Singleton getters should work even with a pending exception.
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  env_->hasPendingException = true;
  env_->pendingException = hermes::vm::HermesValue::encodeUndefinedValue();

  napi_value val;
  EXPECT_EQ(napi_ok, napi_get_undefined(env_, &val));
  EXPECT_NE(nullptr, val);

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ExceptionSafeGetNull) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  env_->hasPendingException = true;
  env_->pendingException = hermes::vm::HermesValue::encodeUndefinedValue();

  napi_value val;
  EXPECT_EQ(napi_ok, napi_get_null(env_, &val));
  EXPECT_NE(nullptr, val);

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ExceptionSafeGetBoolean) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  env_->hasPendingException = true;
  env_->pendingException = hermes::vm::HermesValue::encodeUndefinedValue();

  napi_value val;
  EXPECT_EQ(napi_ok, napi_get_boolean(env_, true, &val));
  EXPECT_NE(nullptr, val);

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ExceptionSafeGetGlobal) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  env_->hasPendingException = true;
  env_->pendingException = hermes::vm::HermesValue::encodeUndefinedValue();

  napi_value val;
  EXPECT_EQ(napi_ok, napi_get_global(env_, &val));
  EXPECT_NE(nullptr, val);

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ExceptionSafeNumberCreation) {
  // Number creation should work even with a pending exception.
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  env_->hasPendingException = true;
  env_->pendingException = hermes::vm::HermesValue::encodeUndefinedValue();

  napi_value val;
  EXPECT_EQ(napi_ok, napi_create_double(env_, 3.14, &val));
  EXPECT_EQ(napi_ok, napi_create_int32(env_, 42, &val));
  EXPECT_EQ(napi_ok, napi_create_uint32(env_, 100u, &val));
  EXPECT_EQ(napi_ok, napi_create_int64(env_, 999, &val));

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ExceptionSafeNumberExtraction) {
  // Number extraction should work even with a pending exception.
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value val;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 3.14, &val));

  env_->hasPendingException = true;
  env_->pendingException = hermes::vm::HermesValue::encodeUndefinedValue();

  double d;
  EXPECT_EQ(napi_ok, napi_get_value_double(env_, val, &d));
  EXPECT_DOUBLE_EQ(3.14, d);

  int32_t i32;
  EXPECT_EQ(napi_ok, napi_get_value_int32(env_, val, &i32));

  uint32_t u32;
  EXPECT_EQ(napi_ok, napi_get_value_uint32(env_, val, &u32));

  int64_t i64;
  EXPECT_EQ(napi_ok, napi_get_value_int64(env_, val, &i64));

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ExceptionSafeBoolExtraction) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value val;
  ASSERT_EQ(napi_ok, napi_get_boolean(env_, true, &val));

  env_->hasPendingException = true;
  env_->pendingException = hermes::vm::HermesValue::encodeUndefinedValue();

  bool b;
  EXPECT_EQ(napi_ok, napi_get_value_bool(env_, val, &b));
  EXPECT_TRUE(b);

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ExceptionSafeTypeof) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value val;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 1.0, &val));

  env_->hasPendingException = true;
  env_->pendingException = hermes::vm::HermesValue::encodeUndefinedValue();

  napi_valuetype type;
  EXPECT_EQ(napi_ok, napi_typeof(env_, val, &type));
  EXPECT_EQ(napi_number, type);

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ExceptionSafeStringExtraction) {
  // String extraction should work even with a pending exception.
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value val;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &val));

  env_->hasPendingException = true;
  env_->pendingException = hermes::vm::HermesValue::encodeUndefinedValue();

  size_t len;
  EXPECT_EQ(napi_ok, napi_get_value_string_utf8(env_, val, nullptr, 0, &len));
  EXPECT_EQ(5u, len);

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

//===========================================================================
// Pending exception with GC root safety
//===========================================================================

TEST_F(NapiTestFixture, PendingExceptionIsGCRooted) {
  // Verify that a pending exception value is kept alive across GC.
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  // Create a string that will be our "exception".
  napi_value str;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "test error", 10, &str));

  // Set it as the pending exception.
  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(str);
  env_->pendingException = *phv;
  env_->hasPendingException = true;

  // Close the handle scope — the string is no longer rooted by the
  // scope, but should still be alive because the pendingException
  // field is a GC root.
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));

  // Force a GC.
  env_->runtime.collect("test");

  // Verify the pending exception is still valid.
  ASSERT_TRUE(env_->hasPendingException);
  ASSERT_TRUE(env_->pendingException.isString());

  // Retrieve and verify the string contents.
  auto *strPrim =
      hermes::vm::vmcast<hermes::vm::StringPrimitive>(env_->pendingException);
  EXPECT_EQ(10u, strPrim->getStringLength());

  // Clean up.
  env_->hasPendingException = false;
}

//===========================================================================
// napi_throw
//===========================================================================

TEST_F(NapiTestFixture, ThrowNullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_throw(nullptr, nullptr));
}

TEST_F(NapiTestFixture, ThrowNullError) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  EXPECT_EQ(napi_invalid_arg, napi_throw(env_, nullptr));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ThrowRejectsWhenExceptionPending) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  // Set a pending exception manually.
  env_->hasPendingException = true;
  env_->pendingException = HermesValue::encodeUndefinedValue();

  napi_value val;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 42.0, &val));
  EXPECT_EQ(napi_pending_exception, napi_throw(env_, val));

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ThrowSetsException) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  // Create a value and throw it.
  napi_value val;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 42.0, &val));
  EXPECT_EQ(napi_ok, napi_throw(env_, val));

  // Verify exception is pending.
  EXPECT_TRUE(env_->hasPendingException);
  EXPECT_TRUE(env_->pendingException.isNumber());
  EXPECT_EQ(42.0, env_->pendingException.getNumber());

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ThrowStringValue) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  // Throw a string value.
  napi_value str;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "custom error", 12, &str));
  EXPECT_EQ(napi_ok, napi_throw(env_, str));

  EXPECT_TRUE(env_->hasPendingException);
  EXPECT_TRUE(env_->pendingException.isString());

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

//===========================================================================
// napi_throw_error
//===========================================================================

TEST_F(NapiTestFixture, ThrowErrorNullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_throw_error(nullptr, nullptr, "msg"));
}

TEST_F(NapiTestFixture, ThrowErrorNullMsg) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  EXPECT_EQ(napi_invalid_arg, napi_throw_error(env_, nullptr, nullptr));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ThrowErrorRejectsWhenExceptionPending) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  env_->hasPendingException = true;
  env_->pendingException = HermesValue::encodeUndefinedValue();

  EXPECT_EQ(
      napi_pending_exception, napi_throw_error(env_, nullptr, "test error"));

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ThrowErrorSetsErrorObject) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  EXPECT_EQ(napi_ok, napi_throw_error(env_, nullptr, "test error"));

  // Verify exception is pending and is a JSError.
  EXPECT_TRUE(env_->hasPendingException);
  EXPECT_TRUE(env_->pendingException.isObject());
  EXPECT_TRUE(hermes::vm::vmisa<JSError>(env_->pendingException));

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ThrowErrorWithCode) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  EXPECT_EQ(napi_ok, napi_throw_error(env_, "ERR_TEST", "test error"));

  EXPECT_TRUE(env_->hasPendingException);
  EXPECT_TRUE(hermes::vm::vmisa<JSError>(env_->pendingException));

  // Verify the "code" property is set by reading it through VM APIs.
  {
    hermes::vm::GCScope gcScope(env_->runtime);
    auto errHandle = hermes::vm::Handle<>(&env_->pendingException);

    // Look up the "code" symbol.
    auto codeSymRes = env_->runtime.getIdentifierTable().getSymbolID(
        env_->runtime, hermes::vm::ASCIIRef{"code", 4});
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, codeSymRes.getStatus());

    // Get the "code" property.
    auto codeRes = JSObject::getNamed_RJS(
        hermes::vm::Handle<JSObject>::vmcast(errHandle),
        env_->runtime,
        *codeSymRes);
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, codeRes.getStatus());

    // Verify it's the string "ERR_TEST".
    auto codeVal = codeRes->get();
    ASSERT_TRUE(codeVal.isString());
    auto *codeStr = hermes::vm::vmcast<StringPrimitive>(codeVal);
    EXPECT_TRUE(codeStr->isASCII());
    auto ref = codeStr->getStringRef<char>();
    EXPECT_EQ(std::string("ERR_TEST"), std::string(ref.data(), ref.size()));
  }

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ThrowErrorMessageIsCorrect) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  EXPECT_EQ(napi_ok, napi_throw_error(env_, nullptr, "hello world"));

  EXPECT_TRUE(env_->hasPendingException);

  // Verify the "message" property.
  {
    hermes::vm::GCScope gcScope(env_->runtime);
    auto errHandle = hermes::vm::Handle<>(&env_->pendingException);

    auto msgRes = JSObject::getNamed_RJS(
        hermes::vm::Handle<JSObject>::vmcast(errHandle),
        env_->runtime,
        hermes::vm::Predefined::getSymbolID(hermes::vm::Predefined::message));
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, msgRes.getStatus());

    auto msgVal = msgRes->get();
    ASSERT_TRUE(msgVal.isString());
    auto *msgStr = hermes::vm::vmcast<StringPrimitive>(msgVal);
    EXPECT_TRUE(msgStr->isASCII());
    auto ref = msgStr->getStringRef<char>();
    EXPECT_EQ(std::string("hello world"), std::string(ref.data(), ref.size()));
  }

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

//===========================================================================
// napi_throw_type_error
//===========================================================================

TEST_F(NapiTestFixture, ThrowTypeErrorNullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_throw_type_error(nullptr, nullptr, "msg"));
}

TEST_F(NapiTestFixture, ThrowTypeErrorNullMsg) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  EXPECT_EQ(napi_invalid_arg, napi_throw_type_error(env_, nullptr, nullptr));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ThrowTypeErrorSetsTypeError) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  EXPECT_EQ(napi_ok, napi_throw_type_error(env_, nullptr, "bad type"));

  EXPECT_TRUE(env_->hasPendingException);
  EXPECT_TRUE(env_->pendingException.isObject());
  EXPECT_TRUE(hermes::vm::vmisa<JSError>(env_->pendingException));

  // Verify it has the TypeError prototype chain by checking "name".
  {
    hermes::vm::GCScope gcScope(env_->runtime);
    auto errHandle = hermes::vm::Handle<>(&env_->pendingException);

    auto nameRes = JSObject::getNamed_RJS(
        hermes::vm::Handle<JSObject>::vmcast(errHandle),
        env_->runtime,
        hermes::vm::Predefined::getSymbolID(hermes::vm::Predefined::name));
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, nameRes.getStatus());

    auto nameVal = nameRes->get();
    ASSERT_TRUE(nameVal.isString());
    auto *nameStr = hermes::vm::vmcast<StringPrimitive>(nameVal);
    auto ref = nameStr->getStringRef<char>();
    EXPECT_EQ(std::string("TypeError"), std::string(ref.data(), ref.size()));
  }

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ThrowTypeErrorWithCode) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  EXPECT_EQ(napi_ok, napi_throw_type_error(env_, "ERR_TYPE", "type error"));

  EXPECT_TRUE(env_->hasPendingException);

  // Verify the "code" property.
  {
    hermes::vm::GCScope gcScope(env_->runtime);
    auto errHandle = hermes::vm::Handle<>(&env_->pendingException);
    auto codeSymRes = env_->runtime.getIdentifierTable().getSymbolID(
        env_->runtime, hermes::vm::ASCIIRef{"code", 4});
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, codeSymRes.getStatus());

    auto codeRes = JSObject::getNamed_RJS(
        hermes::vm::Handle<JSObject>::vmcast(errHandle),
        env_->runtime,
        *codeSymRes);
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, codeRes.getStatus());

    auto codeVal = codeRes->get();
    ASSERT_TRUE(codeVal.isString());
    auto *codeStr = hermes::vm::vmcast<StringPrimitive>(codeVal);
    auto ref = codeStr->getStringRef<char>();
    EXPECT_EQ(std::string("ERR_TYPE"), std::string(ref.data(), ref.size()));
  }

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

//===========================================================================
// napi_throw_range_error
//===========================================================================

TEST_F(NapiTestFixture, ThrowRangeErrorNullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_throw_range_error(nullptr, nullptr, "msg"));
}

TEST_F(NapiTestFixture, ThrowRangeErrorNullMsg) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  EXPECT_EQ(napi_invalid_arg, napi_throw_range_error(env_, nullptr, nullptr));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ThrowRangeErrorSetsRangeError) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  EXPECT_EQ(napi_ok, napi_throw_range_error(env_, nullptr, "out of range"));

  EXPECT_TRUE(env_->hasPendingException);
  EXPECT_TRUE(env_->pendingException.isObject());
  EXPECT_TRUE(hermes::vm::vmisa<JSError>(env_->pendingException));

  // Verify it has the RangeError prototype by checking "name".
  {
    hermes::vm::GCScope gcScope(env_->runtime);
    auto errHandle = hermes::vm::Handle<>(&env_->pendingException);

    auto nameRes = JSObject::getNamed_RJS(
        hermes::vm::Handle<JSObject>::vmcast(errHandle),
        env_->runtime,
        hermes::vm::Predefined::getSymbolID(hermes::vm::Predefined::name));
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, nameRes.getStatus());

    auto nameVal = nameRes->get();
    ASSERT_TRUE(nameVal.isString());
    auto *nameStr = hermes::vm::vmcast<StringPrimitive>(nameVal);
    auto ref = nameStr->getStringRef<char>();
    EXPECT_EQ(std::string("RangeError"), std::string(ref.data(), ref.size()));
  }

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ThrowRangeErrorWithCode) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  EXPECT_EQ(napi_ok, napi_throw_range_error(env_, "ERR_RANGE", "range error"));

  EXPECT_TRUE(env_->hasPendingException);

  // Verify both "message" and "code" properties.
  {
    hermes::vm::GCScope gcScope(env_->runtime);
    auto errHandle = hermes::vm::Handle<>(&env_->pendingException);

    // Check message.
    auto msgRes = JSObject::getNamed_RJS(
        hermes::vm::Handle<JSObject>::vmcast(errHandle),
        env_->runtime,
        hermes::vm::Predefined::getSymbolID(hermes::vm::Predefined::message));
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, msgRes.getStatus());
    auto msgVal = msgRes->get();
    ASSERT_TRUE(msgVal.isString());
    auto *msgStr = hermes::vm::vmcast<StringPrimitive>(msgVal);
    auto msgRef = msgStr->getStringRef<char>();
    EXPECT_EQ(
        std::string("range error"), std::string(msgRef.data(), msgRef.size()));

    // Check code.
    auto codeSymRes = env_->runtime.getIdentifierTable().getSymbolID(
        env_->runtime, hermes::vm::ASCIIRef{"code", 4});
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, codeSymRes.getStatus());
    auto codeRes = JSObject::getNamed_RJS(
        hermes::vm::Handle<JSObject>::vmcast(errHandle),
        env_->runtime,
        *codeSymRes);
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, codeRes.getStatus());
    auto codeVal = codeRes->get();
    ASSERT_TRUE(codeVal.isString());
    auto *codeStr = hermes::vm::vmcast<StringPrimitive>(codeVal);
    auto codeRef = codeStr->getStringRef<char>();
    EXPECT_EQ(
        std::string("ERR_RANGE"), std::string(codeRef.data(), codeRef.size()));
  }

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ThrowErrorWithNullCode) {
  // Verify that passing null for code omits the code property.
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  EXPECT_EQ(napi_ok, napi_throw_error(env_, nullptr, "no code"));

  EXPECT_TRUE(env_->hasPendingException);

  // Verify the "code" property is undefined (not set).
  {
    hermes::vm::GCScope gcScope(env_->runtime);
    auto errHandle = hermes::vm::Handle<>(&env_->pendingException);
    auto codeSymRes = env_->runtime.getIdentifierTable().getSymbolID(
        env_->runtime, hermes::vm::ASCIIRef{"code", 4});
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, codeSymRes.getStatus());

    auto codeRes = JSObject::getNamed_RJS(
        hermes::vm::Handle<JSObject>::vmcast(errHandle),
        env_->runtime,
        *codeSymRes);
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, codeRes.getStatus());

    // When code is null, the property should not exist (returns
    // undefined).
    EXPECT_TRUE(codeRes->get().isUndefined());
  }

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ThrowErrorExceptionSurvivesGC) {
  // Verify that a thrown error object is properly GC-rooted.
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  EXPECT_EQ(napi_ok, napi_throw_error(env_, "GC_TEST", "gc test error"));

  EXPECT_TRUE(env_->hasPendingException);

  // Close handle scope — the error is only rooted by
  // pendingException.
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));

  // Force GC.
  env_->runtime.collect("test");

  // Verify the error object survived GC.
  ASSERT_TRUE(env_->hasPendingException);
  ASSERT_TRUE(env_->pendingException.isObject());
  ASSERT_TRUE(hermes::vm::vmisa<JSError>(env_->pendingException));

  // Verify the message survived.
  {
    hermes::vm::GCScope gcScope(env_->runtime);
    auto errHandle = hermes::vm::Handle<>(&env_->pendingException);

    auto msgRes = JSObject::getNamed_RJS(
        hermes::vm::Handle<JSObject>::vmcast(errHandle),
        env_->runtime,
        hermes::vm::Predefined::getSymbolID(hermes::vm::Predefined::message));
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, msgRes.getStatus());
    auto msgVal = msgRes->get();
    ASSERT_TRUE(msgVal.isString());
    auto *msgStr = hermes::vm::vmcast<StringPrimitive>(msgVal);
    auto ref = msgStr->getStringRef<char>();
    EXPECT_EQ(
        std::string("gc test error"), std::string(ref.data(), ref.size()));
  }

  env_->hasPendingException = false;
}

//===========================================================================
// napi_is_exception_pending
//===========================================================================

TEST_F(NapiTestFixture, IsExceptionPendingNullEnv) {
  bool result = false;
  EXPECT_EQ(napi_invalid_arg, napi_is_exception_pending(nullptr, &result));
  EXPECT_FALSE(result); // Not modified.
}

TEST_F(NapiTestFixture, IsExceptionPendingNullResult) {
  EXPECT_EQ(napi_invalid_arg, napi_is_exception_pending(env_, nullptr));
}

TEST_F(NapiTestFixture, IsExceptionPendingReturnsFalseWhenNone) {
  bool result = true; // Initialize to true to verify it gets set.
  EXPECT_EQ(napi_ok, napi_is_exception_pending(env_, &result));
  EXPECT_FALSE(result);
}

TEST_F(NapiTestFixture, IsExceptionPendingReturnsTrueWhenPending) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  // Throw an error to set the pending exception.
  ASSERT_EQ(napi_ok, napi_throw_error(env_, nullptr, "test"));

  bool result = false;
  EXPECT_EQ(napi_ok, napi_is_exception_pending(env_, &result));
  EXPECT_TRUE(result);

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, IsExceptionPendingWorksWithPendingException) {
  // This function is exception-safe — verify it works even with
  // a pending exception.
  env_->hasPendingException = true;
  env_->pendingException = HermesValue::encodeUndefinedValue();

  bool result = false;
  EXPECT_EQ(napi_ok, napi_is_exception_pending(env_, &result));
  EXPECT_TRUE(result);

  env_->hasPendingException = false;
}

//===========================================================================
// napi_get_and_clear_last_exception
//===========================================================================

TEST_F(NapiTestFixture, GetAndClearLastExceptionNullEnv) {
  napi_value result;
  EXPECT_EQ(
      napi_invalid_arg, napi_get_and_clear_last_exception(nullptr, &result));
}

TEST_F(NapiTestFixture, GetAndClearLastExceptionNullResult) {
  EXPECT_EQ(napi_invalid_arg, napi_get_and_clear_last_exception(env_, nullptr));
}

TEST_F(NapiTestFixture, GetAndClearNoExceptionReturnsUndefined) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  // When there is no pending exception, should return undefined.
  napi_value result;
  EXPECT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &result));
  ASSERT_NE(nullptr, result);

  napi_valuetype type;
  EXPECT_EQ(napi_ok, napi_typeof(env_, result, &type));
  EXPECT_EQ(napi_undefined, type);

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, GetAndClearReturnsExceptionAndClears) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  // Throw an error.
  ASSERT_EQ(napi_ok, napi_throw_error(env_, nullptr, "test exception"));
  ASSERT_TRUE(env_->hasPendingException);

  // Get and clear the exception.
  napi_value result;
  EXPECT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &result));
  ASSERT_NE(nullptr, result);

  // Exception should be cleared now.
  EXPECT_FALSE(env_->hasPendingException);

  // The returned value should be an object (JSError).
  napi_valuetype type;
  EXPECT_EQ(napi_ok, napi_typeof(env_, result, &type));
  EXPECT_EQ(napi_object, type);

  // Verify it's actually a JSError by checking via VM APIs.
  auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(result);
  EXPECT_TRUE(hermes::vm::vmisa<JSError>(*phv));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, GetAndClearReturnsCorrectExceptionValue) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  // Throw a number (not an error object).
  napi_value num;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 42.0, &num));
  ASSERT_EQ(napi_ok, napi_throw(env_, num));

  // Get and clear.
  napi_value result;
  EXPECT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &result));
  EXPECT_FALSE(env_->hasPendingException);

  // Verify the returned value is 42.0.
  double d;
  EXPECT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(42.0, d);

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, GetAndClearAllowsSubsequentAPICalls) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  // Throw an error.
  ASSERT_EQ(napi_ok, napi_throw_error(env_, nullptr, "blocking error"));
  ASSERT_TRUE(env_->hasPendingException);

  // While exception is pending, NAPI_PREAMBLE functions should fail.
  EXPECT_EQ(napi_pending_exception, napi_throw_error(env_, nullptr, "second"));

  // Clear the exception.
  napi_value exc;
  EXPECT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &exc));
  EXPECT_FALSE(env_->hasPendingException);

  // Now NAPI_PREAMBLE functions should work again.
  EXPECT_EQ(napi_ok, napi_throw_error(env_, nullptr, "now it works"));
  EXPECT_TRUE(env_->hasPendingException);

  // Clean up.
  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, GetAndClearExceptionSurvivesInScope) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  // Throw a string value.
  napi_value str;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "error msg", 9, &str));
  ASSERT_EQ(napi_ok, napi_throw(env_, str));

  // Get and clear — the returned value should be in the current scope.
  napi_value result;
  EXPECT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &result));
  EXPECT_FALSE(env_->hasPendingException);

  // Force GC — the result should survive because it's in the scope.
  env_->runtime.collect("test");

  // Verify the string is still valid.
  size_t len;
  EXPECT_EQ(
      napi_ok, napi_get_value_string_utf8(env_, result, nullptr, 0, &len));
  EXPECT_EQ(9u, len);

  char buf[16];
  EXPECT_EQ(
      napi_ok,
      napi_get_value_string_utf8(env_, result, buf, sizeof(buf), &len));
  EXPECT_EQ(9u, len);
  EXPECT_STREQ("error msg", buf);

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, GetAndClearCalledTwice) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  // Throw a value.
  napi_value val;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 99.0, &val));
  ASSERT_EQ(napi_ok, napi_throw(env_, val));

  // First call should return the exception.
  napi_value result1;
  EXPECT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &result1));
  EXPECT_FALSE(env_->hasPendingException);

  double d;
  EXPECT_EQ(napi_ok, napi_get_value_double(env_, result1, &d));
  EXPECT_EQ(99.0, d);

  // Second call should return undefined since no exception is pending.
  napi_value result2;
  EXPECT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &result2));

  napi_valuetype type;
  EXPECT_EQ(napi_ok, napi_typeof(env_, result2, &type));
  EXPECT_EQ(napi_undefined, type);

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

//===========================================================================
// Helper: read a named string property from an error object using VM APIs.
//===========================================================================

static std::string
getStringProperty(napi_env env, napi_value obj, hermes::vm::SymbolID sym) {
  using namespace hermes::vm;
  auto *phv = reinterpret_cast<PinnedHermesValue *>(obj);
  GCScope gcScope(env->runtime);
  hermes::vm::Handle<> handle(phv);
  auto res = JSObject::getNamed_RJS(
      Handle<JSObject>::vmcast(handle), env->runtime, sym);
  assert(res != ExecutionStatus::EXCEPTION);
  auto val = res->get();
  if (!val.isString())
    return "";
  auto *str = vmcast<StringPrimitive>(val);
  if (str->isASCII()) {
    auto ref = str->getStringRef<char>();
    return std::string(ref.data(), ref.size());
  }
  std::string result;
  hermes::convertUTF16ToUTF8WithReplacements(
      result, str->getStringRef<char16_t>());
  return result;
}

//===========================================================================
// napi_create_error
//===========================================================================

TEST_F(NapiTestFixture, CreateErrorNullEnv) {
  EXPECT_EQ(
      napi_invalid_arg, napi_create_error(nullptr, nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, CreateErrorNullMsg) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value result;
  EXPECT_EQ(
      napi_invalid_arg, napi_create_error(env_, nullptr, nullptr, &result));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, CreateErrorNullResult) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value msg;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "err", 3, &msg));
  EXPECT_EQ(napi_invalid_arg, napi_create_error(env_, nullptr, msg, nullptr));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, CreateErrorMsgMustBeString) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value num;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 42.0, &num));
  napi_value result;
  EXPECT_EQ(
      napi_string_expected, napi_create_error(env_, nullptr, num, &result));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, CreateErrorCodeMustBeString) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value msg;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "err", 3, &msg));
  napi_value num;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 42.0, &num));
  napi_value result;
  EXPECT_EQ(napi_string_expected, napi_create_error(env_, num, msg, &result));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, CreateErrorRejectsWhenExceptionPending) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  env_->hasPendingException = true;
  env_->pendingException = HermesValue::encodeUndefinedValue();

  napi_value msg;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "err", 3, &msg));
  napi_value result;
  EXPECT_EQ(
      napi_pending_exception, napi_create_error(env_, nullptr, msg, &result));

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, CreateErrorBasic) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value msg;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "test error", 10, &msg));

  napi_value error;
  EXPECT_EQ(napi_ok, napi_create_error(env_, nullptr, msg, &error));
  ASSERT_NE(nullptr, error);

  // Should be an object.
  napi_valuetype type;
  EXPECT_EQ(napi_ok, napi_typeof(env_, error, &type));
  EXPECT_EQ(napi_object, type);

  // Should be recognized by napi_is_error.
  bool isError = false;
  EXPECT_EQ(napi_ok, napi_is_error(env_, error, &isError));
  EXPECT_TRUE(isError);

  // Should NOT be set as pending exception.
  EXPECT_FALSE(env_->hasPendingException);

  // Verify the message property.
  EXPECT_EQ(
      "test error",
      getStringProperty(
          env_,
          error,
          hermes::vm::Predefined::getSymbolID(
              hermes::vm::Predefined::message)));

  // Verify the name is "Error".
  EXPECT_EQ(
      "Error",
      getStringProperty(
          env_,
          error,
          hermes::vm::Predefined::getSymbolID(hermes::vm::Predefined::name)));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, CreateErrorWithCode) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value msg;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "coded error", 11, &msg));
  napi_value code;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "ERR_TEST", 8, &code));

  napi_value error;
  EXPECT_EQ(napi_ok, napi_create_error(env_, code, msg, &error));
  ASSERT_NE(nullptr, error);

  // Verify message.
  EXPECT_EQ(
      "coded error",
      getStringProperty(
          env_,
          error,
          hermes::vm::Predefined::getSymbolID(
              hermes::vm::Predefined::message)));

  // Verify code property.
  {
    hermes::vm::GCScope gcScope(env_->runtime);
    auto codeSymRes = env_->runtime.getIdentifierTable().getSymbolID(
        env_->runtime, hermes::vm::ASCIIRef{"code", 4});
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, codeSymRes.getStatus());
    EXPECT_EQ("ERR_TEST", getStringProperty(env_, error, *codeSymRes));
  }

  // Should not be pending.
  EXPECT_FALSE(env_->hasPendingException);

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, CreateErrorWithNullCode) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value msg;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "no code", 7, &msg));

  napi_value error;
  EXPECT_EQ(napi_ok, napi_create_error(env_, nullptr, msg, &error));

  // Verify code property is undefined.
  {
    hermes::vm::GCScope gcScope(env_->runtime);
    auto codeSymRes = env_->runtime.getIdentifierTable().getSymbolID(
        env_->runtime, hermes::vm::ASCIIRef{"code", 4});
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, codeSymRes.getStatus());
    auto *phv = reinterpret_cast<hermes::vm::PinnedHermesValue *>(error);
    hermes::vm::Handle<> handle(phv);
    auto res = hermes::vm::JSObject::getNamed_RJS(
        hermes::vm::Handle<JSObject>::vmcast(handle),
        env_->runtime,
        *codeSymRes);
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, res.getStatus());
    EXPECT_TRUE(res->get().isUndefined());
  }

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

//===========================================================================
// napi_create_type_error
//===========================================================================

TEST_F(NapiTestFixture, CreateTypeErrorNullEnv) {
  EXPECT_EQ(
      napi_invalid_arg,
      napi_create_type_error(nullptr, nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, CreateTypeErrorBasic) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value msg;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "type err", 8, &msg));

  napi_value error;
  EXPECT_EQ(napi_ok, napi_create_type_error(env_, nullptr, msg, &error));
  ASSERT_NE(nullptr, error);

  // Should be recognized as an error.
  bool isError = false;
  EXPECT_EQ(napi_ok, napi_is_error(env_, error, &isError));
  EXPECT_TRUE(isError);

  // Should NOT be set as pending exception.
  EXPECT_FALSE(env_->hasPendingException);

  // Verify name is "TypeError".
  EXPECT_EQ(
      "TypeError",
      getStringProperty(
          env_,
          error,
          hermes::vm::Predefined::getSymbolID(hermes::vm::Predefined::name)));

  // Verify message.
  EXPECT_EQ(
      "type err",
      getStringProperty(
          env_,
          error,
          hermes::vm::Predefined::getSymbolID(
              hermes::vm::Predefined::message)));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, CreateTypeErrorWithCode) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value msg;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "msg", 3, &msg));
  napi_value code;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "ERR_TYPE", 8, &code));

  napi_value error;
  EXPECT_EQ(napi_ok, napi_create_type_error(env_, code, msg, &error));

  // Verify code.
  {
    hermes::vm::GCScope gcScope(env_->runtime);
    auto codeSymRes = env_->runtime.getIdentifierTable().getSymbolID(
        env_->runtime, hermes::vm::ASCIIRef{"code", 4});
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, codeSymRes.getStatus());
    EXPECT_EQ("ERR_TYPE", getStringProperty(env_, error, *codeSymRes));
  }

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, CreateTypeErrorMsgMustBeString) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value num;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 1.0, &num));
  napi_value result;
  EXPECT_EQ(
      napi_string_expected,
      napi_create_type_error(env_, nullptr, num, &result));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

//===========================================================================
// napi_create_range_error
//===========================================================================

TEST_F(NapiTestFixture, CreateRangeErrorNullEnv) {
  EXPECT_EQ(
      napi_invalid_arg,
      napi_create_range_error(nullptr, nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, CreateRangeErrorBasic) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value msg;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "range err", 9, &msg));

  napi_value error;
  EXPECT_EQ(napi_ok, napi_create_range_error(env_, nullptr, msg, &error));
  ASSERT_NE(nullptr, error);

  // Should be recognized as an error.
  bool isError = false;
  EXPECT_EQ(napi_ok, napi_is_error(env_, error, &isError));
  EXPECT_TRUE(isError);

  // Should NOT be set as pending exception.
  EXPECT_FALSE(env_->hasPendingException);

  // Verify name is "RangeError".
  EXPECT_EQ(
      "RangeError",
      getStringProperty(
          env_,
          error,
          hermes::vm::Predefined::getSymbolID(hermes::vm::Predefined::name)));

  // Verify message.
  EXPECT_EQ(
      "range err",
      getStringProperty(
          env_,
          error,
          hermes::vm::Predefined::getSymbolID(
              hermes::vm::Predefined::message)));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, CreateRangeErrorWithCode) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value msg;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "msg", 3, &msg));
  napi_value code;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "ERR_RANGE", 9, &code));

  napi_value error;
  EXPECT_EQ(napi_ok, napi_create_range_error(env_, code, msg, &error));

  // Verify code.
  {
    hermes::vm::GCScope gcScope(env_->runtime);
    auto codeSymRes = env_->runtime.getIdentifierTable().getSymbolID(
        env_->runtime, hermes::vm::ASCIIRef{"code", 4});
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, codeSymRes.getStatus());
    EXPECT_EQ("ERR_RANGE", getStringProperty(env_, error, *codeSymRes));
  }

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, CreateRangeErrorMsgMustBeString) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value num;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 1.0, &num));
  napi_value result;
  EXPECT_EQ(
      napi_string_expected,
      napi_create_range_error(env_, nullptr, num, &result));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

//===========================================================================
// node_api_throw_syntax_error
//===========================================================================

TEST_F(NapiTestFixture, ThrowSyntaxErrorNullEnv) {
  EXPECT_EQ(
      napi_invalid_arg, node_api_throw_syntax_error(nullptr, nullptr, "msg"));
}

TEST_F(NapiTestFixture, ThrowSyntaxErrorNullMsg) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  EXPECT_EQ(
      napi_invalid_arg, node_api_throw_syntax_error(env_, nullptr, nullptr));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ThrowSyntaxErrorSetsSyntaxError) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  EXPECT_EQ(napi_ok, node_api_throw_syntax_error(env_, nullptr, "bad syntax"));

  EXPECT_TRUE(env_->hasPendingException);
  EXPECT_TRUE(env_->pendingException.isObject());
  EXPECT_TRUE(hermes::vm::vmisa<JSError>(env_->pendingException));

  // Verify it has the SyntaxError prototype by checking "name".
  {
    hermes::vm::GCScope gcScope(env_->runtime);
    auto errHandle = hermes::vm::Handle<>(&env_->pendingException);

    auto nameRes = JSObject::getNamed_RJS(
        hermes::vm::Handle<JSObject>::vmcast(errHandle),
        env_->runtime,
        hermes::vm::Predefined::getSymbolID(hermes::vm::Predefined::name));
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, nameRes.getStatus());

    auto nameVal = nameRes->get();
    ASSERT_TRUE(nameVal.isString());
    auto *nameStr = hermes::vm::vmcast<StringPrimitive>(nameVal);
    auto ref = nameStr->getStringRef<char>();
    EXPECT_EQ(std::string("SyntaxError"), std::string(ref.data(), ref.size()));
  }

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ThrowSyntaxErrorWithCode) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  EXPECT_EQ(
      napi_ok, node_api_throw_syntax_error(env_, "ERR_SYNTAX", "syntax error"));

  EXPECT_TRUE(env_->hasPendingException);

  // Verify the "code" property.
  {
    hermes::vm::GCScope gcScope(env_->runtime);
    auto errHandle = hermes::vm::Handle<>(&env_->pendingException);
    auto codeSymRes = env_->runtime.getIdentifierTable().getSymbolID(
        env_->runtime, hermes::vm::ASCIIRef{"code", 4});
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, codeSymRes.getStatus());

    auto codeRes = JSObject::getNamed_RJS(
        hermes::vm::Handle<JSObject>::vmcast(errHandle),
        env_->runtime,
        *codeSymRes);
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, codeRes.getStatus());

    auto codeVal = codeRes->get();
    ASSERT_TRUE(codeVal.isString());
    auto *codeStr = hermes::vm::vmcast<StringPrimitive>(codeVal);
    auto ref = codeStr->getStringRef<char>();
    EXPECT_EQ(std::string("ERR_SYNTAX"), std::string(ref.data(), ref.size()));
  }

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, ThrowSyntaxErrorRejectsWhenExceptionPending) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  env_->hasPendingException = true;
  env_->pendingException = HermesValue::encodeUndefinedValue();

  EXPECT_EQ(
      napi_pending_exception,
      node_api_throw_syntax_error(env_, nullptr, "test"));

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

//===========================================================================
// node_api_create_syntax_error
//===========================================================================

TEST_F(NapiTestFixture, CreateSyntaxErrorNullEnv) {
  EXPECT_EQ(
      napi_invalid_arg,
      node_api_create_syntax_error(nullptr, nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, CreateSyntaxErrorBasic) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value msg;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "syntax err", 10, &msg));

  napi_value error;
  EXPECT_EQ(napi_ok, node_api_create_syntax_error(env_, nullptr, msg, &error));
  ASSERT_NE(nullptr, error);

  // Should be recognized as an error.
  bool isError = false;
  EXPECT_EQ(napi_ok, napi_is_error(env_, error, &isError));
  EXPECT_TRUE(isError);

  // Should NOT be set as pending exception.
  EXPECT_FALSE(env_->hasPendingException);

  // Verify name is "SyntaxError".
  EXPECT_EQ(
      "SyntaxError",
      getStringProperty(
          env_,
          error,
          hermes::vm::Predefined::getSymbolID(hermes::vm::Predefined::name)));

  // Verify message.
  EXPECT_EQ(
      "syntax err",
      getStringProperty(
          env_,
          error,
          hermes::vm::Predefined::getSymbolID(
              hermes::vm::Predefined::message)));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, CreateSyntaxErrorWithCode) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value msg;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "msg", 3, &msg));
  napi_value code;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "ERR_SYNTAX", 10, &code));

  napi_value error;
  EXPECT_EQ(napi_ok, node_api_create_syntax_error(env_, code, msg, &error));

  // Verify code.
  {
    hermes::vm::GCScope gcScope(env_->runtime);
    auto codeSymRes = env_->runtime.getIdentifierTable().getSymbolID(
        env_->runtime, hermes::vm::ASCIIRef{"code", 4});
    ASSERT_NE(hermes::vm::ExecutionStatus::EXCEPTION, codeSymRes.getStatus());
    EXPECT_EQ("ERR_SYNTAX", getStringProperty(env_, error, *codeSymRes));
  }

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, CreateSyntaxErrorMsgMustBeString) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value num;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 1.0, &num));
  napi_value result;
  EXPECT_EQ(
      napi_string_expected,
      node_api_create_syntax_error(env_, nullptr, num, &result));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, CreateSyntaxErrorRejectsWhenExceptionPending) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  env_->hasPendingException = true;
  env_->pendingException = HermesValue::encodeUndefinedValue();

  napi_value msg;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "err", 3, &msg));
  napi_value result;
  EXPECT_EQ(
      napi_pending_exception,
      node_api_create_syntax_error(env_, nullptr, msg, &result));

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, CreateSyntaxErrorCodeMustBeString) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value msg;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "err", 3, &msg));
  napi_value num;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 42.0, &num));
  napi_value result;
  EXPECT_EQ(
      napi_string_expected,
      node_api_create_syntax_error(env_, num, msg, &result));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

//===========================================================================
// napi_is_error
//===========================================================================

TEST_F(NapiTestFixture, IsErrorNullEnv) {
  bool result = false;
  EXPECT_EQ(napi_invalid_arg, napi_is_error(nullptr, nullptr, &result));
}

TEST_F(NapiTestFixture, IsErrorNullValue) {
  bool result = false;
  EXPECT_EQ(napi_invalid_arg, napi_is_error(env_, nullptr, &result));
}

TEST_F(NapiTestFixture, IsErrorNullResult) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value val;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 1.0, &val));
  EXPECT_EQ(napi_invalid_arg, napi_is_error(env_, val, nullptr));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, IsErrorReturnsFalseForNumber) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value val;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 42.0, &val));

  bool isError = true;
  EXPECT_EQ(napi_ok, napi_is_error(env_, val, &isError));
  EXPECT_FALSE(isError);

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, IsErrorReturnsFalseForString) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value val;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &val));

  bool isError = true;
  EXPECT_EQ(napi_ok, napi_is_error(env_, val, &isError));
  EXPECT_FALSE(isError);

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, IsErrorReturnsFalseForUndefined) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value val;
  ASSERT_EQ(napi_ok, napi_get_undefined(env_, &val));

  bool isError = true;
  EXPECT_EQ(napi_ok, napi_is_error(env_, val, &isError));
  EXPECT_FALSE(isError);

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, IsErrorReturnsFalseForNull) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value val;
  ASSERT_EQ(napi_ok, napi_get_null(env_, &val));

  bool isError = true;
  EXPECT_EQ(napi_ok, napi_is_error(env_, val, &isError));
  EXPECT_FALSE(isError);

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, IsErrorReturnsFalseForBoolean) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value val;
  ASSERT_EQ(napi_ok, napi_get_boolean(env_, true, &val));

  bool isError = true;
  EXPECT_EQ(napi_ok, napi_is_error(env_, val, &isError));
  EXPECT_FALSE(isError);

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, IsErrorReturnsTrueForCreatedError) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value msg;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "err", 3, &msg));
  napi_value error;
  ASSERT_EQ(napi_ok, napi_create_error(env_, nullptr, msg, &error));

  bool isError = false;
  EXPECT_EQ(napi_ok, napi_is_error(env_, error, &isError));
  EXPECT_TRUE(isError);

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, IsErrorReturnsTrueForThrownError) {
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  // Throw an error and retrieve it.
  ASSERT_EQ(napi_ok, napi_throw_error(env_, nullptr, "thrown error"));

  napi_value exc;
  ASSERT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &exc));

  bool isError = false;
  EXPECT_EQ(napi_ok, napi_is_error(env_, exc, &isError));
  EXPECT_TRUE(isError);

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, IsErrorWorksWithPendingException) {
  // napi_is_error is exception-safe (uses CHECK_ENV, not
  // NAPI_PREAMBLE).
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value msg;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "err", 3, &msg));
  napi_value error;
  ASSERT_EQ(napi_ok, napi_create_error(env_, nullptr, msg, &error));

  env_->hasPendingException = true;
  env_->pendingException = HermesValue::encodeUndefinedValue();

  bool isError = false;
  EXPECT_EQ(napi_ok, napi_is_error(env_, error, &isError));
  EXPECT_TRUE(isError);

  env_->hasPendingException = false;
  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

TEST_F(NapiTestFixture, CreateErrorThenThrow) {
  // Verify the create-then-throw workflow: create error without
  // throwing, inspect it, then throw it.
  napi_handle_scope scope;
  ASSERT_EQ(napi_ok, napi_open_handle_scope(env_, &scope));

  napi_value msg;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "deferred", 8, &msg));
  napi_value code;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "ERR_DEFERRED", 12, &code));

  napi_value error;
  ASSERT_EQ(napi_ok, napi_create_error(env_, code, msg, &error));

  // Not pending yet.
  EXPECT_FALSE(env_->hasPendingException);

  // Now throw it.
  EXPECT_EQ(napi_ok, napi_throw(env_, error));
  EXPECT_TRUE(env_->hasPendingException);

  // Get and clear.
  napi_value exc;
  ASSERT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &exc));

  // Verify it's the same error.
  bool isError = false;
  EXPECT_EQ(napi_ok, napi_is_error(env_, exc, &isError));
  EXPECT_TRUE(isError);

  EXPECT_EQ(
      "deferred",
      getStringProperty(
          env_,
          exc,
          hermes::vm::Predefined::getSymbolID(
              hermes::vm::Predefined::message)));

  ASSERT_EQ(napi_ok, napi_close_handle_scope(env_, scope));
}

} // namespace

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NapiTestFixture.h"

#include "hermes/VM/BigIntPrimitive.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/HermesValue.h"
#include "hermes/VM/JSDate.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/Predefined.h"
#include "hermes/VM/StringPrimitive.h"

#include <cmath>
#include <limits>

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

//===========================================================================
// napi_get_undefined
//===========================================================================

TEST_F(NapiTestFixture, GetUndefined_Basic) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_undefined(env_, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isUndefined());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetUndefined_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_get_undefined(nullptr, nullptr));
}

TEST_F(NapiTestFixture, GetUndefined_NullResult) {
  EXPECT_EQ(napi_invalid_arg, napi_get_undefined(env_, nullptr));
}

TEST_F(NapiTestFixture, GetUndefined_MultipleCallsReturnDistinctHandles) {
  napi_handle_scope scope = openScope(env_);

  napi_value r1 = nullptr;
  napi_value r2 = nullptr;
  ASSERT_EQ(napi_ok, napi_get_undefined(env_, &r1));
  ASSERT_EQ(napi_ok, napi_get_undefined(env_, &r2));

  // Each call allocates a new handle scope slot.
  EXPECT_NE(r1, r2);

  // Both should be undefined.
  auto *p1 = reinterpret_cast<PinnedHermesValue *>(r1);
  auto *p2 = reinterpret_cast<PinnedHermesValue *>(r2);
  EXPECT_TRUE(p1->isUndefined());
  EXPECT_TRUE(p2->isUndefined());

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_null
//===========================================================================

TEST_F(NapiTestFixture, GetNull_Basic) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_null(env_, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNull());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetNull_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_get_null(nullptr, nullptr));
}

TEST_F(NapiTestFixture, GetNull_NullResult) {
  EXPECT_EQ(napi_invalid_arg, napi_get_null(env_, nullptr));
}

//===========================================================================
// napi_get_boolean
//===========================================================================

TEST_F(NapiTestFixture, GetBoolean_True) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_boolean(env_, true, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isBool());
  EXPECT_TRUE(phv->getBool());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetBoolean_False) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_boolean(env_, false, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isBool());
  EXPECT_FALSE(phv->getBool());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetBoolean_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_get_boolean(nullptr, true, nullptr));
}

TEST_F(NapiTestFixture, GetBoolean_NullResult) {
  EXPECT_EQ(napi_invalid_arg, napi_get_boolean(env_, true, nullptr));
}

//===========================================================================
// napi_get_global
//===========================================================================

TEST_F(NapiTestFixture, GetGlobal_Basic) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_global(env_, &result));
  ASSERT_NE(nullptr, result);

  // The global object should be a JSObject.
  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isObject());
  EXPECT_TRUE(vmisa<JSObject>(*phv));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetGlobal_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_get_global(nullptr, nullptr));
}

TEST_F(NapiTestFixture, GetGlobal_NullResult) {
  EXPECT_EQ(napi_invalid_arg, napi_get_global(env_, nullptr));
}

TEST_F(NapiTestFixture, GetGlobal_SameObjectOnMultipleCalls) {
  napi_handle_scope scope = openScope(env_);

  napi_value r1 = nullptr;
  napi_value r2 = nullptr;
  ASSERT_EQ(napi_ok, napi_get_global(env_, &r1));
  ASSERT_EQ(napi_ok, napi_get_global(env_, &r2));

  // Different handle scope slots...
  EXPECT_NE(r1, r2);

  // ...but same underlying JS object.
  auto *p1 = reinterpret_cast<PinnedHermesValue *>(r1);
  auto *p2 = reinterpret_cast<PinnedHermesValue *>(r2);
  EXPECT_EQ(p1->getObject(), p2->getObject());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetGlobal_SurvivesGC) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_global(env_, &result));

  // Trigger GC — the global object should survive.
  collectAndDrain();

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isObject());
  EXPECT_TRUE(vmisa<JSObject>(*phv));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetGlobal_ClearsError) {
  napi_handle_scope scope = openScope(env_);

  // Set an error state first.
  napi_set_last_error(env_, napi_generic_failure);
  EXPECT_EQ(napi_generic_failure, env_->last_error.error_code);

  // napi_get_global should clear the error on success.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_global(env_, &result));
  EXPECT_EQ(napi_ok, env_->last_error.error_code);

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_double
//===========================================================================

TEST_F(NapiTestFixture, CreateDouble_Zero) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 0.0, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_EQ(0.0, phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDouble_PositiveValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 3.14, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_DOUBLE_EQ(3.14, phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDouble_NegativeValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, -1.5, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_DOUBLE_EQ(-1.5, phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDouble_NaN) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, NAN, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_TRUE(std::isnan(phv->getNumber()));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDouble_Infinity) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, INFINITY, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_TRUE(std::isinf(phv->getNumber()));
  EXPECT_GT(phv->getNumber(), 0);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDouble_NegativeInfinity) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, -INFINITY, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_TRUE(std::isinf(phv->getNumber()));
  EXPECT_LT(phv->getNumber(), 0);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDouble_MaxDouble) {
  napi_handle_scope scope = openScope(env_);

  double maxVal = std::numeric_limits<double>::max();
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, maxVal, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_EQ(maxVal, phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDouble_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_create_double(nullptr, 0.0, nullptr));
}

TEST_F(NapiTestFixture, CreateDouble_NullResult) {
  EXPECT_EQ(napi_invalid_arg, napi_create_double(env_, 0.0, nullptr));
}

//===========================================================================
// napi_create_int32
//===========================================================================

TEST_F(NapiTestFixture, CreateInt32_Zero) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 0, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_EQ(0.0, phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateInt32_PositiveValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_EQ(42.0, phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateInt32_NegativeValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, -1, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_EQ(-1.0, phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateInt32_MaxValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_create_int32(env_, std::numeric_limits<int32_t>::max(), &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_EQ(
      static_cast<double>(std::numeric_limits<int32_t>::max()),
      phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateInt32_MinValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_create_int32(env_, std::numeric_limits<int32_t>::min(), &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_EQ(
      static_cast<double>(std::numeric_limits<int32_t>::min()),
      phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateInt32_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_create_int32(nullptr, 0, nullptr));
}

TEST_F(NapiTestFixture, CreateInt32_NullResult) {
  EXPECT_EQ(napi_invalid_arg, napi_create_int32(env_, 0, nullptr));
}

//===========================================================================
// napi_create_uint32
//===========================================================================

TEST_F(NapiTestFixture, CreateUint32_Zero) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_uint32(env_, 0, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_EQ(0.0, phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateUint32_PositiveValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_uint32(env_, 123, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_EQ(123.0, phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateUint32_MaxValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_create_uint32(env_, std::numeric_limits<uint32_t>::max(), &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_EQ(
      static_cast<double>(std::numeric_limits<uint32_t>::max()),
      phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateUint32_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_create_uint32(nullptr, 0, nullptr));
}

TEST_F(NapiTestFixture, CreateUint32_NullResult) {
  EXPECT_EQ(napi_invalid_arg, napi_create_uint32(env_, 0, nullptr));
}

//===========================================================================
// napi_create_int64
//===========================================================================

TEST_F(NapiTestFixture, CreateInt64_Zero) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int64(env_, 0, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_EQ(0.0, phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateInt64_PositiveValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int64(env_, 12345, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_EQ(12345.0, phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateInt64_NegativeValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int64(env_, -99, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_EQ(-99.0, phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateInt64_LargeValueLosesPrecision) {
  napi_handle_scope scope = openScope(env_);

  // 2^53 + 1 cannot be represented exactly as a double.
  int64_t bigVal = (1LL << 53) + 1;
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int64(env_, bigVal, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  // The double representation will round to 2^53.
  EXPECT_EQ(static_cast<double>(bigVal), phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateInt64_MaxValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_create_int64(env_, std::numeric_limits<int64_t>::max(), &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_EQ(
      static_cast<double>(std::numeric_limits<int64_t>::max()),
      phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateInt64_MinValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_create_int64(env_, std::numeric_limits<int64_t>::min(), &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isNumber());
  EXPECT_EQ(
      static_cast<double>(std::numeric_limits<int64_t>::min()),
      phv->getNumber());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateInt64_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_create_int64(nullptr, 0, nullptr));
}

TEST_F(NapiTestFixture, CreateInt64_NullResult) {
  EXPECT_EQ(napi_invalid_arg, napi_create_int64(env_, 0, nullptr));
}

//===========================================================================
// napi_get_value_double
//===========================================================================

TEST_F(NapiTestFixture, GetValueDouble_FromDouble) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 3.14, &val));

  double result = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, val, &result));
  EXPECT_DOUBLE_EQ(3.14, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueDouble_FromInt32) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &val));

  double result = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, val, &result));
  EXPECT_EQ(42.0, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueDouble_NaN) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, NAN, &val));

  double result = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, val, &result));
  EXPECT_TRUE(std::isnan(result));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueDouble_Infinity) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, INFINITY, &val));

  double result = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, val, &result));
  EXPECT_TRUE(std::isinf(result));
  EXPECT_GT(result, 0);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueDouble_TypeErrorOnBoolean) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_get_boolean(env_, true, &val));

  double result = 0;
  EXPECT_EQ(napi_number_expected, napi_get_value_double(env_, val, &result));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueDouble_TypeErrorOnUndefined) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_get_undefined(env_, &val));

  double result = 0;
  EXPECT_EQ(napi_number_expected, napi_get_value_double(env_, val, &result));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueDouble_TypeErrorOnNull) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_get_null(env_, &val));

  double result = 0;
  EXPECT_EQ(napi_number_expected, napi_get_value_double(env_, val, &result));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueDouble_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_get_value_double(nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, GetValueDouble_NullValue) {
  double result = 0;
  EXPECT_EQ(napi_invalid_arg, napi_get_value_double(env_, nullptr, &result));
}

TEST_F(NapiTestFixture, GetValueDouble_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 1.0, &val));

  EXPECT_EQ(napi_invalid_arg, napi_get_value_double(env_, val, nullptr));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_int32
//===========================================================================

TEST_F(NapiTestFixture, GetValueInt32_Roundtrip) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, -42, &val));

  int32_t result = 0;
  ASSERT_EQ(napi_ok, napi_get_value_int32(env_, val, &result));
  EXPECT_EQ(-42, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueInt32_MaxValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  int32_t max = std::numeric_limits<int32_t>::max();
  ASSERT_EQ(napi_ok, napi_create_int32(env_, max, &val));

  int32_t result = 0;
  ASSERT_EQ(napi_ok, napi_get_value_int32(env_, val, &result));
  EXPECT_EQ(max, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueInt32_MinValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  int32_t min = std::numeric_limits<int32_t>::min();
  ASSERT_EQ(napi_ok, napi_create_int32(env_, min, &val));

  int32_t result = 0;
  ASSERT_EQ(napi_ok, napi_get_value_int32(env_, val, &result));
  EXPECT_EQ(min, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueInt32_TruncatesDouble) {
  napi_handle_scope scope = openScope(env_);

  // 3.7 should truncate to 3 per ES ToInt32.
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 3.7, &val));

  int32_t result = 0;
  ASSERT_EQ(napi_ok, napi_get_value_int32(env_, val, &result));
  EXPECT_EQ(3, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueInt32_NaNBecomesZero) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, NAN, &val));

  int32_t result = 99;
  ASSERT_EQ(napi_ok, napi_get_value_int32(env_, val, &result));
  EXPECT_EQ(0, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueInt32_InfinityBecomesZero) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, INFINITY, &val));

  int32_t result = 99;
  ASSERT_EQ(napi_ok, napi_get_value_int32(env_, val, &result));
  EXPECT_EQ(0, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueInt32_LargeDoubleWraps) {
  napi_handle_scope scope = openScope(env_);

  // 2^32 + 1 = 4294967297.0 should wrap to 1 per ToInt32.
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 4294967297.0, &val));

  int32_t result = 0;
  ASSERT_EQ(napi_ok, napi_get_value_int32(env_, val, &result));
  EXPECT_EQ(1, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueInt32_TypeErrorOnBoolean) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_get_boolean(env_, true, &val));

  int32_t result = 0;
  EXPECT_EQ(napi_number_expected, napi_get_value_int32(env_, val, &result));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueInt32_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_get_value_int32(nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, GetValueInt32_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));

  EXPECT_EQ(napi_invalid_arg, napi_get_value_int32(env_, val, nullptr));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_uint32
//===========================================================================

TEST_F(NapiTestFixture, GetValueUint32_Roundtrip) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_uint32(env_, 123, &val));

  uint32_t result = 0;
  ASSERT_EQ(napi_ok, napi_get_value_uint32(env_, val, &result));
  EXPECT_EQ(123u, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueUint32_MaxValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  uint32_t max = std::numeric_limits<uint32_t>::max();
  ASSERT_EQ(napi_ok, napi_create_uint32(env_, max, &val));

  uint32_t result = 0;
  ASSERT_EQ(napi_ok, napi_get_value_uint32(env_, val, &result));
  EXPECT_EQ(max, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueUint32_FromNegativeInt32) {
  napi_handle_scope scope = openScope(env_);

  // -1 as a double, interpreted via ToUint32, should be 0xFFFFFFFF.
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, -1.0, &val));

  uint32_t result = 0;
  ASSERT_EQ(napi_ok, napi_get_value_uint32(env_, val, &result));
  EXPECT_EQ(0xFFFFFFFFu, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueUint32_NaNBecomesZero) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, NAN, &val));

  uint32_t result = 99;
  ASSERT_EQ(napi_ok, napi_get_value_uint32(env_, val, &result));
  EXPECT_EQ(0u, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueUint32_TypeErrorOnNull) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_get_null(env_, &val));

  uint32_t result = 0;
  EXPECT_EQ(napi_number_expected, napi_get_value_uint32(env_, val, &result));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueUint32_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_get_value_uint32(nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, GetValueUint32_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_uint32(env_, 1, &val));

  EXPECT_EQ(napi_invalid_arg, napi_get_value_uint32(env_, val, nullptr));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_int64
//===========================================================================

TEST_F(NapiTestFixture, GetValueInt64_Roundtrip) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int64(env_, 12345, &val));

  int64_t result = 0;
  ASSERT_EQ(napi_ok, napi_get_value_int64(env_, val, &result));
  EXPECT_EQ(12345, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueInt64_NegativeValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int64(env_, -99, &val));

  int64_t result = 0;
  ASSERT_EQ(napi_ok, napi_get_value_int64(env_, val, &result));
  EXPECT_EQ(-99, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueInt64_FromDouble) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 3.7, &val));

  int64_t result = 0;
  ASSERT_EQ(napi_ok, napi_get_value_int64(env_, val, &result));
  // Truncation toward zero.
  EXPECT_EQ(3, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueInt64_NaNBecomesZero) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, NAN, &val));

  int64_t result = 99;
  ASSERT_EQ(napi_ok, napi_get_value_int64(env_, val, &result));
  EXPECT_EQ(0, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueInt64_InfinityBecomesZero) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, INFINITY, &val));

  int64_t result = 99;
  ASSERT_EQ(napi_ok, napi_get_value_int64(env_, val, &result));
  EXPECT_EQ(0, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueInt64_NegativeInfinityBecomesZero) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, -INFINITY, &val));

  int64_t result = 99;
  ASSERT_EQ(napi_ok, napi_get_value_int64(env_, val, &result));
  EXPECT_EQ(0, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueInt64_TypeErrorOnUndefined) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_get_undefined(env_, &val));

  int64_t result = 0;
  EXPECT_EQ(napi_number_expected, napi_get_value_int64(env_, val, &result));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueInt64_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_get_value_int64(nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, GetValueInt64_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int64(env_, 1, &val));

  EXPECT_EQ(napi_invalid_arg, napi_get_value_int64(env_, val, nullptr));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_bool
//===========================================================================

TEST_F(NapiTestFixture, GetValueBool_True) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_get_boolean(env_, true, &val));

  bool result = false;
  ASSERT_EQ(napi_ok, napi_get_value_bool(env_, val, &result));
  EXPECT_TRUE(result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueBool_False) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_get_boolean(env_, false, &val));

  bool result = true;
  ASSERT_EQ(napi_ok, napi_get_value_bool(env_, val, &result));
  EXPECT_FALSE(result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueBool_TypeErrorOnNumber) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));

  bool result = false;
  EXPECT_EQ(napi_boolean_expected, napi_get_value_bool(env_, val, &result));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueBool_TypeErrorOnUndefined) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_get_undefined(env_, &val));

  bool result = false;
  EXPECT_EQ(napi_boolean_expected, napi_get_value_bool(env_, val, &result));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueBool_TypeErrorOnNull) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_get_null(env_, &val));

  bool result = false;
  EXPECT_EQ(napi_boolean_expected, napi_get_value_bool(env_, val, &result));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueBool_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_get_value_bool(nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, GetValueBool_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_get_boolean(env_, true, &val));

  EXPECT_EQ(napi_invalid_arg, napi_get_value_bool(env_, val, nullptr));

  closeScope(env_, scope);
}

//===========================================================================
// napi_typeof
//===========================================================================

TEST_F(NapiTestFixture, Typeof_Undefined) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_get_undefined(env_, &val));

  napi_valuetype result;
  ASSERT_EQ(napi_ok, napi_typeof(env_, val, &result));
  EXPECT_EQ(napi_undefined, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Typeof_Null) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_get_null(env_, &val));

  napi_valuetype result;
  ASSERT_EQ(napi_ok, napi_typeof(env_, val, &result));
  EXPECT_EQ(napi_null, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Typeof_BooleanTrue) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_get_boolean(env_, true, &val));

  napi_valuetype result;
  ASSERT_EQ(napi_ok, napi_typeof(env_, val, &result));
  EXPECT_EQ(napi_boolean, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Typeof_BooleanFalse) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_get_boolean(env_, false, &val));

  napi_valuetype result;
  ASSERT_EQ(napi_ok, napi_typeof(env_, val, &result));
  EXPECT_EQ(napi_boolean, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Typeof_NumberDouble) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 3.14, &val));

  napi_valuetype result;
  ASSERT_EQ(napi_ok, napi_typeof(env_, val, &result));
  EXPECT_EQ(napi_number, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Typeof_NumberInt32) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &val));

  napi_valuetype result;
  ASSERT_EQ(napi_ok, napi_typeof(env_, val, &result));
  EXPECT_EQ(napi_number, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Typeof_NumberNaN) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, NAN, &val));

  napi_valuetype result;
  ASSERT_EQ(napi_ok, napi_typeof(env_, val, &result));
  EXPECT_EQ(napi_number, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Typeof_String) {
  napi_handle_scope scope = openScope(env_);

  // Create a string via Hermes VM internals since napi_create_string_utf8
  // is not yet implemented.
  napi_value val;
  {
    auto strRes = StringPrimitive::createEfficient(
        *rt_, llvh::ArrayRef<char>("hello", 5));
    ASSERT_NE(ExecutionStatus::EXCEPTION, strRes.getStatus());
    val = env_->addToCurrentScope(*strRes);
  }

  napi_valuetype result;
  ASSERT_EQ(napi_ok, napi_typeof(env_, val, &result));
  EXPECT_EQ(napi_string, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Typeof_Symbol) {
  napi_handle_scope scope = openScope(env_);

  // Create a symbol value by encoding a predefined SymbolID.
  napi_value val = env_->addToCurrentScope(
      HermesValue::encodeSymbolValue(
          Predefined::getSymbolID(Predefined::emptyString)));

  napi_valuetype result;
  ASSERT_EQ(napi_ok, napi_typeof(env_, val, &result));
  EXPECT_EQ(napi_symbol, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Typeof_Object) {
  napi_handle_scope scope = openScope(env_);

  // Create a plain JSObject via VM internals.
  napi_value val;
  {
    auto obj = JSObject::create(*rt_);
    val = env_->addToCurrentScope(obj.getHermesValue());
  }

  napi_valuetype result;
  ASSERT_EQ(napi_ok, napi_typeof(env_, val, &result));
  EXPECT_EQ(napi_object, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Typeof_Function) {
  napi_handle_scope scope = openScope(env_);

  // Create a NativeFunction via VM internals.
  napi_value val;
  {
    auto fn = NativeFunction::create(
        *rt_,
        nullptr, // context
        nullptr, // functionPtr (ok for typeof test)
        Predefined::getSymbolID(Predefined::emptyString),
        0 /* paramCount */);
    val = env_->addToCurrentScope(fn.getHermesValue());
  }

  napi_valuetype result;
  ASSERT_EQ(napi_ok, napi_typeof(env_, val, &result));
  EXPECT_EQ(napi_function, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Typeof_BigInt) {
  napi_handle_scope scope = openScope(env_);

  // Create a BigInt via VM internals.
  napi_value val;
  {
    auto biRes = BigIntPrimitive::fromSigned(*rt_, 42);
    ASSERT_NE(ExecutionStatus::EXCEPTION, biRes.getStatus());
    val = env_->addToCurrentScope(*biRes);
  }

  napi_valuetype result;
  ASSERT_EQ(napi_ok, napi_typeof(env_, val, &result));
  EXPECT_EQ(napi_bigint, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Typeof_GlobalIsObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_get_global(env_, &val));

  napi_valuetype result;
  ASSERT_EQ(napi_ok, napi_typeof(env_, val, &result));
  EXPECT_EQ(napi_object, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Typeof_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_typeof(nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, Typeof_NullValue) {
  napi_valuetype result;
  EXPECT_EQ(napi_invalid_arg, napi_typeof(env_, nullptr, &result));
}

TEST_F(NapiTestFixture, Typeof_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_get_undefined(env_, &val));

  EXPECT_EQ(napi_invalid_arg, napi_typeof(env_, val, nullptr));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Typeof_ClearsError) {
  napi_handle_scope scope = openScope(env_);

  // Set an error state first.
  napi_set_last_error(env_, napi_generic_failure);
  EXPECT_EQ(napi_generic_failure, env_->last_error.error_code);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_get_undefined(env_, &val));

  napi_valuetype result;
  ASSERT_EQ(napi_ok, napi_typeof(env_, val, &result));
  EXPECT_EQ(napi_ok, env_->last_error.error_code);

  closeScope(env_, scope);
}

//===========================================================================
// Symbol operations
//===========================================================================

TEST_F(NapiTestFixture, CreateSymbol_NoDescription) {
  napi_handle_scope scope = openScope(env_);

  napi_value sym = nullptr;
  ASSERT_EQ(napi_ok, napi_create_symbol(env_, nullptr, &sym));
  ASSERT_NE(nullptr, sym);

  // Verify typeof returns napi_symbol.
  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, sym, &type));
  EXPECT_EQ(napi_symbol, type);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateSymbol_WithDescription) {
  napi_handle_scope scope = openScope(env_);

  // Create a description string.
  napi_value desc = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_create_string_utf8(env_, "mySymbol", NAPI_AUTO_LENGTH, &desc));

  napi_value sym = nullptr;
  ASSERT_EQ(napi_ok, napi_create_symbol(env_, desc, &sym));
  ASSERT_NE(nullptr, sym);

  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, sym, &type));
  EXPECT_EQ(napi_symbol, type);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateSymbol_TwoSymbolsNotEqual) {
  napi_handle_scope scope = openScope(env_);

  // Create a description string.
  napi_value desc = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_string_utf8(env_, "same", NAPI_AUTO_LENGTH, &desc));

  napi_value sym1 = nullptr;
  napi_value sym2 = nullptr;
  ASSERT_EQ(napi_ok, napi_create_symbol(env_, desc, &sym1));
  ASSERT_EQ(napi_ok, napi_create_symbol(env_, desc, &sym2));

  // Two symbols with the same description must NOT be strictly equal.
  bool equal = true;
  ASSERT_EQ(napi_ok, napi_strict_equals(env_, sym1, sym2, &equal));
  EXPECT_FALSE(equal);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateSymbol_NonStringDescriptionFails) {
  napi_handle_scope scope = openScope(env_);

  // Pass a number as description — should fail with napi_string_expected.
  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 42.0, &num));

  napi_value sym = nullptr;
  EXPECT_EQ(napi_string_expected, napi_create_symbol(env_, num, &sym));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SymbolFor_SameKeySameSymbol) {
  napi_handle_scope scope = openScope(env_);

  napi_value sym1 = nullptr;
  napi_value sym2 = nullptr;
  ASSERT_EQ(
      napi_ok,
      node_api_symbol_for(env_, "shared_key", NAPI_AUTO_LENGTH, &sym1));
  ASSERT_EQ(
      napi_ok,
      node_api_symbol_for(env_, "shared_key", NAPI_AUTO_LENGTH, &sym2));

  // Symbol.for() with the same key must return the same symbol.
  bool equal = false;
  ASSERT_EQ(napi_ok, napi_strict_equals(env_, sym1, sym2, &equal));
  EXPECT_TRUE(equal);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SymbolFor_DifferentKeyDifferentSymbol) {
  napi_handle_scope scope = openScope(env_);

  napi_value sym1 = nullptr;
  napi_value sym2 = nullptr;
  ASSERT_EQ(
      napi_ok, node_api_symbol_for(env_, "key_a", NAPI_AUTO_LENGTH, &sym1));
  ASSERT_EQ(
      napi_ok, node_api_symbol_for(env_, "key_b", NAPI_AUTO_LENGTH, &sym2));

  // Different keys must produce different symbols.
  bool equal = true;
  ASSERT_EQ(napi_ok, napi_strict_equals(env_, sym1, sym2, &equal));
  EXPECT_FALSE(equal);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SymbolFor_TypeofIsSymbol) {
  napi_handle_scope scope = openScope(env_);

  napi_value sym = nullptr;
  ASSERT_EQ(napi_ok, node_api_symbol_for(env_, "test", NAPI_AUTO_LENGTH, &sym));

  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, sym, &type));
  EXPECT_EQ(napi_symbol, type);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SymbolFor_ExplicitLength) {
  napi_handle_scope scope = openScope(env_);

  // Use explicit length to only take the first 3 chars "abc".
  napi_value sym1 = nullptr;
  napi_value sym2 = nullptr;
  ASSERT_EQ(napi_ok, node_api_symbol_for(env_, "abcdef", 3, &sym1));
  ASSERT_EQ(napi_ok, node_api_symbol_for(env_, "abc", 3, &sym2));

  bool equal = false;
  ASSERT_EQ(napi_ok, napi_strict_equals(env_, sym1, sym2, &equal));
  EXPECT_TRUE(equal);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateSymbol_VsSymbolFor_NotEqual) {
  napi_handle_scope scope = openScope(env_);

  // Symbol() and Symbol.for() with the same description produce
  // different symbols.
  napi_value desc = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_string_utf8(env_, "test", NAPI_AUTO_LENGTH, &desc));

  napi_value sym1 = nullptr;
  napi_value sym2 = nullptr;
  ASSERT_EQ(napi_ok, napi_create_symbol(env_, desc, &sym1));
  ASSERT_EQ(
      napi_ok, node_api_symbol_for(env_, "test", NAPI_AUTO_LENGTH, &sym2));

  bool equal = true;
  ASSERT_EQ(napi_ok, napi_strict_equals(env_, sym1, sym2, &equal));
  EXPECT_FALSE(equal);

  closeScope(env_, scope);
}

//===========================================================================
// Date operations
//===========================================================================

TEST_F(NapiTestFixture, CreateDate_Basic) {
  napi_handle_scope scope = openScope(env_);

  double time = 1234567890123.0; // milliseconds since epoch
  napi_value date = nullptr;
  ASSERT_EQ(napi_ok, napi_create_date(env_, time, &date));
  ASSERT_NE(nullptr, date);

  // The result should be a Date object.
  auto *phv = reinterpret_cast<PinnedHermesValue *>(date);
  EXPECT_TRUE(phv->isObject());
  EXPECT_TRUE(vmisa<JSDate>(*phv));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDate_Zero) {
  napi_handle_scope scope = openScope(env_);

  napi_value date = nullptr;
  ASSERT_EQ(napi_ok, napi_create_date(env_, 0.0, &date));
  ASSERT_NE(nullptr, date);

  double result = 0;
  ASSERT_EQ(napi_ok, napi_get_date_value(env_, date, &result));
  EXPECT_EQ(0.0, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDate_NegativeTime) {
  napi_handle_scope scope = openScope(env_);

  // Negative time represents dates before epoch.
  double time = -86400000.0; // 1 day before epoch
  napi_value date = nullptr;
  ASSERT_EQ(napi_ok, napi_create_date(env_, time, &date));

  double result = 0;
  ASSERT_EQ(napi_ok, napi_get_date_value(env_, date, &result));
  EXPECT_DOUBLE_EQ(time, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDate_NaN) {
  napi_handle_scope scope = openScope(env_);

  // NaN is valid for Date (represents "Invalid Date").
  napi_value date = nullptr;
  ASSERT_EQ(napi_ok, napi_create_date(env_, NAN, &date));

  double result = 0;
  ASSERT_EQ(napi_ok, napi_get_date_value(env_, date, &result));
  EXPECT_TRUE(std::isnan(result));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDate_Roundtrip) {
  napi_handle_scope scope = openScope(env_);

  double time = 1234567890123.0;
  napi_value date = nullptr;
  ASSERT_EQ(napi_ok, napi_create_date(env_, time, &date));

  double result = 0;
  ASSERT_EQ(napi_ok, napi_get_date_value(env_, date, &result));
  EXPECT_DOUBLE_EQ(time, result);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDate_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_create_date(nullptr, 0.0, nullptr));
}

TEST_F(NapiTestFixture, CreateDate_NullResult) {
  EXPECT_EQ(napi_invalid_arg, napi_create_date(env_, 0.0, nullptr));
}

TEST_F(NapiTestFixture, IsDate_DateIsTrue) {
  napi_handle_scope scope = openScope(env_);

  napi_value date = nullptr;
  ASSERT_EQ(napi_ok, napi_create_date(env_, 1000.0, &date));

  bool is_date = false;
  ASSERT_EQ(napi_ok, napi_is_date(env_, date, &is_date));
  EXPECT_TRUE(is_date);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsDate_ObjectIsFalse) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  bool is_date = true;
  ASSERT_EQ(napi_ok, napi_is_date(env_, obj, &is_date));
  EXPECT_FALSE(is_date);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsDate_NumberIsFalse) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 1000.0, &num));

  bool is_date = true;
  ASSERT_EQ(napi_ok, napi_is_date(env_, num, &is_date));
  EXPECT_FALSE(is_date);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsDate_UndefinedIsFalse) {
  napi_handle_scope scope = openScope(env_);

  napi_value undef = nullptr;
  ASSERT_EQ(napi_ok, napi_get_undefined(env_, &undef));

  bool is_date = true;
  ASSERT_EQ(napi_ok, napi_is_date(env_, undef, &is_date));
  EXPECT_FALSE(is_date);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsDate_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_is_date(nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, IsDate_NullValue) {
  bool is_date = false;
  EXPECT_EQ(napi_invalid_arg, napi_is_date(env_, nullptr, &is_date));
}

TEST_F(NapiTestFixture, IsDate_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value date = nullptr;
  ASSERT_EQ(napi_ok, napi_create_date(env_, 0.0, &date));

  EXPECT_EQ(napi_invalid_arg, napi_is_date(env_, date, nullptr));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetDateValue_TypeErrorOnObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  double result = 0;
  EXPECT_EQ(napi_date_expected, napi_get_date_value(env_, obj, &result));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetDateValue_TypeErrorOnNumber) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 42.0, &num));

  double result = 0;
  EXPECT_EQ(napi_date_expected, napi_get_date_value(env_, num, &result));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetDateValue_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_get_date_value(nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, GetDateValue_NullValue) {
  double result = 0;
  EXPECT_EQ(napi_invalid_arg, napi_get_date_value(env_, nullptr, &result));
}

TEST_F(NapiTestFixture, GetDateValue_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value date = nullptr;
  ASSERT_EQ(napi_ok, napi_create_date(env_, 0.0, &date));

  EXPECT_EQ(napi_invalid_arg, napi_get_date_value(env_, date, nullptr));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDate_TypeofIsObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value date = nullptr;
  ASSERT_EQ(napi_ok, napi_create_date(env_, 1000.0, &date));

  // Date typeof should be "object" (not a special NAPI type).
  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, date, &type));
  EXPECT_EQ(napi_object, type);

  closeScope(env_, scope);
}

} // namespace

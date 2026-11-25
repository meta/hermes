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
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSObject.h"
#include "hermes/VM/StringPrimitive.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

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
// napi_create_object
//===========================================================================

TEST_F(NapiTestFixture, CreateObject_Basic) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &result));
  ASSERT_NE(nullptr, result);

  // The result should be an object.
  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isObject());
  EXPECT_TRUE(vmisa<JSObject>(*phv));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateObject_TypeOf) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &result));

  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, result, &type));
  EXPECT_EQ(napi_object, type);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateObject_NullEnv) {
  napi_value result = nullptr;
  EXPECT_EQ(napi_invalid_arg, napi_create_object(nullptr, &result));
}

TEST_F(NapiTestFixture, CreateObject_NullResult) {
  napi_handle_scope scope = openScope(env_);
  EXPECT_EQ(napi_invalid_arg, napi_create_object(env_, nullptr));
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateObject_MultipleDistinct) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj1 = nullptr;
  napi_value obj2 = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj1));
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj2));

  // Each call should create a distinct object.
  auto *phv1 = reinterpret_cast<PinnedHermesValue *>(obj1);
  auto *phv2 = reinterpret_cast<PinnedHermesValue *>(obj2);
  EXPECT_NE(phv1->getObject(), phv2->getObject());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateObject_NotCallable) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // A plain object should not be a function.
  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, obj, &type));
  EXPECT_NE(napi_function, type);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateObject_IsNotError) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // A plain object should not be an error.
  bool isError = true;
  ASSERT_EQ(napi_ok, napi_is_error(env_, obj, &isError));
  EXPECT_FALSE(isError);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateObject_IsNotArray) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // A plain object should not be null, undefined, a number, etc.
  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, obj, &type));
  EXPECT_NE(napi_null, type);
  EXPECT_NE(napi_undefined, type);
  EXPECT_NE(napi_number, type);
  EXPECT_NE(napi_string, type);
  EXPECT_NE(napi_boolean, type);

  closeScope(env_, scope);
}

//===========================================================================
// napi_set_property / napi_get_property
//===========================================================================

TEST_F(NapiTestFixture, SetGetProperty_StringKey) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Create a string key and a number value.
  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &key));
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &val));

  // Set the property.
  ASSERT_EQ(napi_ok, napi_set_property(env_, obj, key, val));

  // Get the property back.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property(env_, obj, key, &result));

  // Verify the value.
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(42.0, d);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetGetProperty_NumericKey) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Create a numeric key and a string value.
  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 0, &key));
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "zero", 4, &val));

  // Set the property.
  ASSERT_EQ(napi_ok, napi_set_property(env_, obj, key, val));

  // Get the property back.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property(env_, obj, key, &result));

  // Verify the value.
  char buf[16];
  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf8(env_, result, buf, 16, &len));
  EXPECT_EQ(4u, len);
  EXPECT_STREQ("zero", buf);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetGetProperty_Overwrite) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "x", 1, &key));

  // Set to 10, then overwrite to 20.
  napi_value val1 = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 10, &val1));
  ASSERT_EQ(napi_ok, napi_set_property(env_, obj, key, val1));

  napi_value val2 = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 20, &val2));
  ASSERT_EQ(napi_ok, napi_set_property(env_, obj, key, val2));

  // Should read the latest value.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property(env_, obj, key, &result));
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(20.0, d);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetProperty_NonExistent) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "missing", 7, &key));

  // Getting a non-existent property should return undefined.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property(env_, obj, key, &result));

  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, result, &type));
  EXPECT_EQ(napi_undefined, type);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetProperty_NullEnv) {
  EXPECT_EQ(
      napi_invalid_arg, napi_set_property(nullptr, nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, SetProperty_NullKey) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));

  EXPECT_EQ(napi_invalid_arg, napi_set_property(env_, obj, nullptr, val));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetProperty_NullValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));
  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "k", 1, &key));

  EXPECT_EQ(napi_invalid_arg, napi_set_property(env_, obj, key, nullptr));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetProperty_NotAnObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &num));
  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "k", 1, &key));
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));

  // Trying to set a property on a non-object should fail.
  EXPECT_EQ(napi_object_expected, napi_set_property(env_, num, key, val));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetProperty_NotAnObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &num));
  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "k", 1, &key));

  napi_value result = nullptr;
  EXPECT_EQ(napi_object_expected, napi_get_property(env_, num, key, &result));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetProperty_NullObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "k", 1, &key));
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));

  EXPECT_EQ(napi_object_expected, napi_set_property(env_, nullptr, key, val));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetProperty_PendingException) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));
  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "k", 1, &key));
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));

  // Set a pending exception.
  napi_value err = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "err", 3, &err));
  ASSERT_EQ(napi_ok, napi_throw(env_, err));

  // Property operations should fail with pending exception.
  EXPECT_EQ(napi_pending_exception, napi_set_property(env_, obj, key, val));

  // Clear the exception.
  napi_value exc = nullptr;
  ASSERT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &exc));

  closeScope(env_, scope);
}

//===========================================================================
// napi_has_property
//===========================================================================

TEST_F(NapiTestFixture, HasProperty_Exists) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "present", 7, &key));
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));
  ASSERT_EQ(napi_ok, napi_set_property(env_, obj, key, val));

  bool hasIt = false;
  ASSERT_EQ(napi_ok, napi_has_property(env_, obj, key, &hasIt));
  EXPECT_TRUE(hasIt);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasProperty_NotExists) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "absent", 6, &key));

  bool hasIt = true;
  ASSERT_EQ(napi_ok, napi_has_property(env_, obj, key, &hasIt));
  EXPECT_FALSE(hasIt);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasProperty_NullEnv) {
  EXPECT_EQ(
      napi_invalid_arg, napi_has_property(nullptr, nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, HasProperty_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));
  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "k", 1, &key));

  EXPECT_EQ(napi_invalid_arg, napi_has_property(env_, obj, key, nullptr));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasProperty_NullKey) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  bool hasIt = false;
  EXPECT_EQ(napi_invalid_arg, napi_has_property(env_, obj, nullptr, &hasIt));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasProperty_NotAnObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &num));
  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "k", 1, &key));

  bool hasIt = false;
  EXPECT_EQ(napi_object_expected, napi_has_property(env_, num, key, &hasIt));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasProperty_NumericKey) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 5, &key));
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "v", 1, &val));
  ASSERT_EQ(napi_ok, napi_set_property(env_, obj, key, val));

  bool hasIt = false;
  ASSERT_EQ(napi_ok, napi_has_property(env_, obj, key, &hasIt));
  EXPECT_TRUE(hasIt);

  // Different numeric key should not exist.
  napi_value otherKey = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 99, &otherKey));
  ASSERT_EQ(napi_ok, napi_has_property(env_, obj, otherKey, &hasIt));
  EXPECT_FALSE(hasIt);

  closeScope(env_, scope);
}

//===========================================================================
// napi_delete_property
//===========================================================================

TEST_F(NapiTestFixture, DeleteProperty_Basic) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "toDelete", 8, &key));
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));
  ASSERT_EQ(napi_ok, napi_set_property(env_, obj, key, val));

  // Verify it exists.
  bool hasIt = false;
  ASSERT_EQ(napi_ok, napi_has_property(env_, obj, key, &hasIt));
  ASSERT_TRUE(hasIt);

  // Delete it.
  bool deleted = false;
  ASSERT_EQ(napi_ok, napi_delete_property(env_, obj, key, &deleted));
  EXPECT_TRUE(deleted);

  // Verify it's gone.
  ASSERT_EQ(napi_ok, napi_has_property(env_, obj, key, &hasIt));
  EXPECT_FALSE(hasIt);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DeleteProperty_NonExistent) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "noSuch", 6, &key));

  // Deleting a non-existent property should succeed (return true).
  bool deleted = false;
  ASSERT_EQ(napi_ok, napi_delete_property(env_, obj, key, &deleted));
  EXPECT_TRUE(deleted);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DeleteProperty_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "toDelete", 8, &key));
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));
  ASSERT_EQ(napi_ok, napi_set_property(env_, obj, key, val));

  // result can be nullptr — should still succeed.
  ASSERT_EQ(napi_ok, napi_delete_property(env_, obj, key, nullptr));

  // Verify it's gone.
  bool hasIt = false;
  ASSERT_EQ(napi_ok, napi_has_property(env_, obj, key, &hasIt));
  EXPECT_FALSE(hasIt);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DeleteProperty_NullKey) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  bool deleted = false;
  EXPECT_EQ(
      napi_invalid_arg, napi_delete_property(env_, obj, nullptr, &deleted));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DeleteProperty_NotAnObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "s", 1, &str));
  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "k", 1, &key));

  bool deleted = false;
  EXPECT_EQ(
      napi_object_expected, napi_delete_property(env_, str, key, &deleted));

  closeScope(env_, scope);
}

//===========================================================================
// napi_set_named_property / napi_get_named_property /
// napi_has_named_property
//===========================================================================

TEST_F(NapiTestFixture, SetGetNamedProperty_Basic) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Set a named property.
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &val));
  ASSERT_EQ(napi_ok, napi_set_named_property(env_, obj, "answer", val));

  // Get it back.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "answer", &result));

  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(42.0, d);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasNamedProperty_Exists) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));
  ASSERT_EQ(napi_ok, napi_set_named_property(env_, obj, "present", val));

  bool hasIt = false;
  ASSERT_EQ(napi_ok, napi_has_named_property(env_, obj, "present", &hasIt));
  EXPECT_TRUE(hasIt);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasNamedProperty_NotExists) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  bool hasIt = true;
  ASSERT_EQ(napi_ok, napi_has_named_property(env_, obj, "absent", &hasIt));
  EXPECT_FALSE(hasIt);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetNamedProperty_Overwrite) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val1 = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 10, &val1));
  ASSERT_EQ(napi_ok, napi_set_named_property(env_, obj, "x", val1));

  napi_value val2 = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 20, &val2));
  ASSERT_EQ(napi_ok, napi_set_named_property(env_, obj, "x", val2));

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "x", &result));
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(20.0, d);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetNamedProperty_NonExistent) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Getting a non-existent named property should return undefined.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "missing", &result));

  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, result, &type));
  EXPECT_EQ(napi_undefined, type);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetNamedProperty_NullEnv) {
  EXPECT_EQ(
      napi_invalid_arg,
      napi_set_named_property(nullptr, nullptr, "k", nullptr));
}

TEST_F(NapiTestFixture, GetNamedProperty_NullEnv) {
  napi_value result = nullptr;
  EXPECT_EQ(
      napi_invalid_arg,
      napi_get_named_property(nullptr, nullptr, "k", &result));
}

TEST_F(NapiTestFixture, HasNamedProperty_NullEnv) {
  bool hasIt = false;
  EXPECT_EQ(
      napi_invalid_arg, napi_has_named_property(nullptr, nullptr, "k", &hasIt));
}

TEST_F(NapiTestFixture, SetNamedProperty_NullName) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));

  EXPECT_EQ(napi_invalid_arg, napi_set_named_property(env_, obj, nullptr, val));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetNamedProperty_NullName) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value result = nullptr;
  EXPECT_EQ(
      napi_invalid_arg, napi_get_named_property(env_, obj, nullptr, &result));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasNamedProperty_NullName) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  bool hasIt = false;
  EXPECT_EQ(
      napi_invalid_arg, napi_has_named_property(env_, obj, nullptr, &hasIt));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetNamedProperty_NullValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  EXPECT_EQ(napi_invalid_arg, napi_set_named_property(env_, obj, "k", nullptr));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetNamedProperty_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  EXPECT_EQ(napi_invalid_arg, napi_get_named_property(env_, obj, "k", nullptr));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasNamedProperty_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  EXPECT_EQ(napi_invalid_arg, napi_has_named_property(env_, obj, "k", nullptr));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetNamedProperty_NotAnObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &num));
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));

  EXPECT_EQ(napi_object_expected, napi_set_named_property(env_, num, "k", val));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetNamedProperty_NotAnObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &num));

  napi_value result = nullptr;
  EXPECT_EQ(
      napi_object_expected, napi_get_named_property(env_, num, "k", &result));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasNamedProperty_NotAnObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &num));

  bool hasIt = false;
  EXPECT_EQ(
      napi_object_expected, napi_has_named_property(env_, num, "k", &hasIt));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetNamedProperty_PendingException) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));

  // Set a pending exception.
  napi_value err = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "err", 3, &err));
  ASSERT_EQ(napi_ok, napi_throw(env_, err));

  EXPECT_EQ(
      napi_pending_exception, napi_set_named_property(env_, obj, "k", val));

  // Clear.
  napi_value exc = nullptr;
  ASSERT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &exc));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, NamedProperty_InteropWithValueKey) {
  // Verify that named property operations and value-key property
  // operations interoperate correctly on the same property.
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Set via named API.
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 100, &val));
  ASSERT_EQ(napi_ok, napi_set_named_property(env_, obj, "shared", val));

  // Read via value-key API.
  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "shared", 6, &key));
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property(env_, obj, key, &result));
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(100.0, d);

  // Has via value-key API.
  bool hasIt = false;
  ASSERT_EQ(napi_ok, napi_has_property(env_, obj, key, &hasIt));
  EXPECT_TRUE(hasIt);

  // Set via value-key API, read via named API.
  napi_value val2 = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 200, &val2));
  ASSERT_EQ(napi_ok, napi_set_property(env_, obj, key, val2));

  napi_value result2 = nullptr;
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "shared", &result2));
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result2, &d));
  EXPECT_EQ(200.0, d);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, NamedProperty_IndexLikeName) {
  // Property names like "0", "1" are index-like — verify they work
  // correctly with the named API.
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 99, &val));
  ASSERT_EQ(napi_ok, napi_set_named_property(env_, obj, "0", val));

  bool hasIt = false;
  ASSERT_EQ(napi_ok, napi_has_named_property(env_, obj, "0", &hasIt));
  EXPECT_TRUE(hasIt);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "0", &result));
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(99.0, d);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, NamedProperty_EmptyName) {
  // Empty string is a valid property name.
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 7, &val));
  ASSERT_EQ(napi_ok, napi_set_named_property(env_, obj, "", val));

  bool hasIt = false;
  ASSERT_EQ(napi_ok, napi_has_named_property(env_, obj, "", &hasIt));
  EXPECT_TRUE(hasIt);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "", &result));
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(7.0, d);

  closeScope(env_, scope);
}

//===========================================================================
// Multiple properties on the same object
//===========================================================================

TEST_F(NapiTestFixture, SetGetProperty_MultipleKeys) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Set three properties with different types of values.
  napi_value keyA = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "a", 1, &keyA));
  napi_value valA = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &valA));
  ASSERT_EQ(napi_ok, napi_set_property(env_, obj, keyA, valA));

  napi_value keyB = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "b", 1, &keyB));
  napi_value valB = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &valB));
  ASSERT_EQ(napi_ok, napi_set_property(env_, obj, keyB, valB));

  napi_value keyC = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "c", 1, &keyC));
  napi_value valC = nullptr;
  ASSERT_EQ(napi_ok, napi_get_boolean(env_, true, &valC));
  ASSERT_EQ(napi_ok, napi_set_property(env_, obj, keyC, valC));

  // Read them back.
  napi_value resultA = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property(env_, obj, keyA, &resultA));
  double dA = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, resultA, &dA));
  EXPECT_EQ(1.0, dA);

  napi_value resultB = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property(env_, obj, keyB, &resultB));
  char buf[16];
  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf8(env_, resultB, buf, 16, &len));
  EXPECT_STREQ("hello", buf);

  napi_value resultC = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property(env_, obj, keyC, &resultC));
  bool bVal = false;
  ASSERT_EQ(napi_ok, napi_get_value_bool(env_, resultC, &bVal));
  EXPECT_TRUE(bVal);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetProperty_ValueIsObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value inner = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &inner));

  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "nested", 6, &key));
  ASSERT_EQ(napi_ok, napi_set_property(env_, obj, key, inner));

  // Read it back and verify it's the same object.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property(env_, obj, key, &result));

  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, result, &type));
  EXPECT_EQ(napi_object, type);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetGetProperty_UndefinedValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "u", 1, &key));

  // Set the property to undefined.
  napi_value undef = nullptr;
  ASSERT_EQ(napi_ok, napi_get_undefined(env_, &undef));
  ASSERT_EQ(napi_ok, napi_set_property(env_, obj, key, undef));

  // The property should exist (has_property returns true).
  bool hasIt = false;
  ASSERT_EQ(napi_ok, napi_has_property(env_, obj, key, &hasIt));
  EXPECT_TRUE(hasIt);

  // The value should be undefined.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property(env_, obj, key, &result));
  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, result, &type));
  EXPECT_EQ(napi_undefined, type);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetGetProperty_NullValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "n", 1, &key));

  // Set the property to null.
  napi_value nullVal = nullptr;
  ASSERT_EQ(napi_ok, napi_get_null(env_, &nullVal));
  ASSERT_EQ(napi_ok, napi_set_property(env_, obj, key, nullVal));

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property(env_, obj, key, &result));
  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, result, &type));
  EXPECT_EQ(napi_null, type);

  closeScope(env_, scope);
}

//===========================================================================
// napi_set_element / napi_get_element / napi_has_element /
// napi_delete_element
//===========================================================================

TEST_F(NapiTestFixture, SetGetElement_Basic) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Set element at index 0.
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &val));
  ASSERT_EQ(napi_ok, napi_set_element(env_, obj, 0, val));

  // Get it back.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_element(env_, obj, 0, &result));

  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(42.0, d);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetGetElement_MultipleIndices) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Set elements at indices 0, 1, 2.
  for (int i = 0; i < 3; ++i) {
    napi_value val = nullptr;
    ASSERT_EQ(napi_ok, napi_create_int32(env_, i * 10, &val));
    ASSERT_EQ(napi_ok, napi_set_element(env_, obj, i, val));
  }

  // Read them back.
  for (int i = 0; i < 3; ++i) {
    napi_value result = nullptr;
    ASSERT_EQ(napi_ok, napi_get_element(env_, obj, i, &result));
    double d = 0;
    ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
    EXPECT_EQ(static_cast<double>(i * 10), d);
  }

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetElement_Overwrite) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val1 = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 10, &val1));
  ASSERT_EQ(napi_ok, napi_set_element(env_, obj, 0, val1));

  napi_value val2 = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 20, &val2));
  ASSERT_EQ(napi_ok, napi_set_element(env_, obj, 0, val2));

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_element(env_, obj, 0, &result));
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(20.0, d);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetElement_NonExistent) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Getting a non-existent element should return undefined.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_element(env_, obj, 99, &result));

  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, result, &type));
  EXPECT_EQ(napi_undefined, type);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasElement_Exists) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));
  ASSERT_EQ(napi_ok, napi_set_element(env_, obj, 5, val));

  bool hasIt = false;
  ASSERT_EQ(napi_ok, napi_has_element(env_, obj, 5, &hasIt));
  EXPECT_TRUE(hasIt);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasElement_NotExists) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  bool hasIt = true;
  ASSERT_EQ(napi_ok, napi_has_element(env_, obj, 0, &hasIt));
  EXPECT_FALSE(hasIt);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DeleteElement_Basic) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));
  ASSERT_EQ(napi_ok, napi_set_element(env_, obj, 3, val));

  // Verify it exists.
  bool hasIt = false;
  ASSERT_EQ(napi_ok, napi_has_element(env_, obj, 3, &hasIt));
  ASSERT_TRUE(hasIt);

  // Delete it.
  bool deleted = false;
  ASSERT_EQ(napi_ok, napi_delete_element(env_, obj, 3, &deleted));
  EXPECT_TRUE(deleted);

  // Verify it's gone.
  ASSERT_EQ(napi_ok, napi_has_element(env_, obj, 3, &hasIt));
  EXPECT_FALSE(hasIt);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DeleteElement_NonExistent) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Deleting a non-existent element should succeed (return true).
  bool deleted = false;
  ASSERT_EQ(napi_ok, napi_delete_element(env_, obj, 99, &deleted));
  EXPECT_TRUE(deleted);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DeleteElement_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));
  ASSERT_EQ(napi_ok, napi_set_element(env_, obj, 7, val));

  // result can be nullptr — should still succeed.
  ASSERT_EQ(napi_ok, napi_delete_element(env_, obj, 7, nullptr));

  // Verify it's gone.
  bool hasIt = false;
  ASSERT_EQ(napi_ok, napi_has_element(env_, obj, 7, &hasIt));
  EXPECT_FALSE(hasIt);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetElement_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_set_element(nullptr, nullptr, 0, nullptr));
}

TEST_F(NapiTestFixture, GetElement_NullEnv) {
  napi_value result = nullptr;
  EXPECT_EQ(napi_invalid_arg, napi_get_element(nullptr, nullptr, 0, &result));
}

TEST_F(NapiTestFixture, HasElement_NullEnv) {
  bool hasIt = false;
  EXPECT_EQ(napi_invalid_arg, napi_has_element(nullptr, nullptr, 0, &hasIt));
}

TEST_F(NapiTestFixture, DeleteElement_NullEnv) {
  bool deleted = false;
  EXPECT_EQ(
      napi_invalid_arg, napi_delete_element(nullptr, nullptr, 0, &deleted));
}

TEST_F(NapiTestFixture, SetElement_NullValue) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  EXPECT_EQ(napi_invalid_arg, napi_set_element(env_, obj, 0, nullptr));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetElement_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  EXPECT_EQ(napi_invalid_arg, napi_get_element(env_, obj, 0, nullptr));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasElement_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  EXPECT_EQ(napi_invalid_arg, napi_has_element(env_, obj, 0, nullptr));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetElement_NotAnObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &num));
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));

  EXPECT_EQ(napi_object_expected, napi_set_element(env_, num, 0, val));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetElement_NotAnObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &num));

  napi_value result = nullptr;
  EXPECT_EQ(napi_object_expected, napi_get_element(env_, num, 0, &result));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasElement_NotAnObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &num));

  bool hasIt = false;
  EXPECT_EQ(napi_object_expected, napi_has_element(env_, num, 0, &hasIt));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DeleteElement_NotAnObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "s", 1, &str));

  bool deleted = false;
  EXPECT_EQ(napi_object_expected, napi_delete_element(env_, str, 0, &deleted));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetElement_NullObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));

  EXPECT_EQ(napi_object_expected, napi_set_element(env_, nullptr, 0, val));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, SetElement_PendingException) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));

  // Set a pending exception.
  napi_value err = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "err", 3, &err));
  ASSERT_EQ(napi_ok, napi_throw(env_, err));

  // Element operations should fail with pending exception.
  EXPECT_EQ(napi_pending_exception, napi_set_element(env_, obj, 0, val));

  // Clear the exception.
  napi_value exc = nullptr;
  ASSERT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &exc));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Element_LargeIndex) {
  // Test with UINT32_MAX index to verify large indices work.
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 77, &val));
  ASSERT_EQ(napi_ok, napi_set_element(env_, obj, UINT32_MAX, val));

  bool hasIt = false;
  ASSERT_EQ(napi_ok, napi_has_element(env_, obj, UINT32_MAX, &hasIt));
  EXPECT_TRUE(hasIt);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_element(env_, obj, UINT32_MAX, &result));
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(77.0, d);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Element_InteropWithValueKeyProperty) {
  // Setting via napi_set_element and reading via napi_get_property
  // with a numeric key should be consistent.
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 55, &val));
  ASSERT_EQ(napi_ok, napi_set_element(env_, obj, 3, val));

  // Read via napi_get_property with numeric key.
  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 3, &key));
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property(env_, obj, key, &result));
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(55.0, d);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Element_InteropWithNamedProperty) {
  // Setting via napi_set_element and reading via
  // napi_get_named_property with an index-like string should be
  // consistent.
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 88, &val));
  ASSERT_EQ(napi_ok, napi_set_element(env_, obj, 2, val));

  // Read via named property "2".
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "2", &result));
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(88.0, d);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Element_SetDifferentValueTypes) {
  // Store different value types at different indices.
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Index 0: number
  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 3.14, &num));
  ASSERT_EQ(napi_ok, napi_set_element(env_, obj, 0, num));

  // Index 1: string
  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hi", 2, &str));
  ASSERT_EQ(napi_ok, napi_set_element(env_, obj, 1, str));

  // Index 2: boolean
  napi_value boolVal = nullptr;
  ASSERT_EQ(napi_ok, napi_get_boolean(env_, false, &boolVal));
  ASSERT_EQ(napi_ok, napi_set_element(env_, obj, 2, boolVal));

  // Index 3: null
  napi_value nullVal = nullptr;
  ASSERT_EQ(napi_ok, napi_get_null(env_, &nullVal));
  ASSERT_EQ(napi_ok, napi_set_element(env_, obj, 3, nullVal));

  // Verify types.
  napi_value r0 = nullptr;
  ASSERT_EQ(napi_ok, napi_get_element(env_, obj, 0, &r0));
  napi_valuetype t0;
  ASSERT_EQ(napi_ok, napi_typeof(env_, r0, &t0));
  EXPECT_EQ(napi_number, t0);

  napi_value r1 = nullptr;
  ASSERT_EQ(napi_ok, napi_get_element(env_, obj, 1, &r1));
  napi_valuetype t1;
  ASSERT_EQ(napi_ok, napi_typeof(env_, r1, &t1));
  EXPECT_EQ(napi_string, t1);

  napi_value r2 = nullptr;
  ASSERT_EQ(napi_ok, napi_get_element(env_, obj, 2, &r2));
  napi_valuetype t2;
  ASSERT_EQ(napi_ok, napi_typeof(env_, r2, &t2));
  EXPECT_EQ(napi_boolean, t2);

  napi_value r3 = nullptr;
  ASSERT_EQ(napi_ok, napi_get_element(env_, obj, 3, &r3));
  napi_valuetype t3;
  ASSERT_EQ(napi_ok, napi_typeof(env_, r3, &t3));
  EXPECT_EQ(napi_null, t3);

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_array
//===========================================================================

TEST_F(NapiTestFixture, CreateArray_Basic) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array(env_, &result));
  ASSERT_NE(nullptr, result);

  // The result should be an array.
  bool isArray = false;
  ASSERT_EQ(napi_ok, napi_is_array(env_, result, &isArray));
  EXPECT_TRUE(isArray);

  // Should also be an object.
  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, result, &type));
  EXPECT_EQ(napi_object, type);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateArray_EmptyLength) {
  napi_handle_scope scope = openScope(env_);

  napi_value arr = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array(env_, &arr));

  // Empty array should have length 0.
  uint32_t length = 99;
  ASSERT_EQ(napi_ok, napi_get_array_length(env_, arr, &length));
  EXPECT_EQ(0u, length);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateArray_NullEnv) {
  napi_value result = nullptr;
  EXPECT_EQ(napi_invalid_arg, napi_create_array(nullptr, &result));
}

TEST_F(NapiTestFixture, CreateArray_NullResult) {
  napi_handle_scope scope = openScope(env_);
  EXPECT_EQ(napi_invalid_arg, napi_create_array(env_, nullptr));
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateArray_MultipleDistinct) {
  napi_handle_scope scope = openScope(env_);

  napi_value arr1 = nullptr;
  napi_value arr2 = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array(env_, &arr1));
  ASSERT_EQ(napi_ok, napi_create_array(env_, &arr2));

  // Each call should create a distinct array.
  auto *phv1 = reinterpret_cast<PinnedHermesValue *>(arr1);
  auto *phv2 = reinterpret_cast<PinnedHermesValue *>(arr2);
  EXPECT_NE(phv1->getObject(), phv2->getObject());

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_array_with_length
//===========================================================================

TEST_F(NapiTestFixture, CreateArrayWithLength_Basic) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array_with_length(env_, 10, &result));
  ASSERT_NE(nullptr, result);

  // Should be an array.
  bool isArray = false;
  ASSERT_EQ(napi_ok, napi_is_array(env_, result, &isArray));
  EXPECT_TRUE(isArray);

  // Should have the specified length.
  uint32_t length = 0;
  ASSERT_EQ(napi_ok, napi_get_array_length(env_, result, &length));
  EXPECT_EQ(10u, length);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateArrayWithLength_Zero) {
  napi_handle_scope scope = openScope(env_);

  napi_value arr = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array_with_length(env_, 0, &arr));

  uint32_t length = 99;
  ASSERT_EQ(napi_ok, napi_get_array_length(env_, arr, &length));
  EXPECT_EQ(0u, length);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateArrayWithLength_NullEnv) {
  napi_value result = nullptr;
  EXPECT_EQ(
      napi_invalid_arg, napi_create_array_with_length(nullptr, 5, &result));
}

TEST_F(NapiTestFixture, CreateArrayWithLength_NullResult) {
  napi_handle_scope scope = openScope(env_);
  EXPECT_EQ(napi_invalid_arg, napi_create_array_with_length(env_, 5, nullptr));
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateArrayWithLength_ElementsUndefined) {
  // Elements in a pre-allocated array should be undefined until set.
  napi_handle_scope scope = openScope(env_);

  napi_value arr = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array_with_length(env_, 3, &arr));

  // Reading an unset element should return undefined.
  napi_value elem = nullptr;
  ASSERT_EQ(napi_ok, napi_get_element(env_, arr, 0, &elem));
  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, elem, &type));
  EXPECT_EQ(napi_undefined, type);

  closeScope(env_, scope);
}

//===========================================================================
// napi_is_array
//===========================================================================

TEST_F(NapiTestFixture, IsArray_Array) {
  napi_handle_scope scope = openScope(env_);

  napi_value arr = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array(env_, &arr));

  bool isArray = false;
  ASSERT_EQ(napi_ok, napi_is_array(env_, arr, &isArray));
  EXPECT_TRUE(isArray);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsArray_PlainObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  bool isArray = true;
  ASSERT_EQ(napi_ok, napi_is_array(env_, obj, &isArray));
  EXPECT_FALSE(isArray);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsArray_Number) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &num));

  bool isArray = true;
  ASSERT_EQ(napi_ok, napi_is_array(env_, num, &isArray));
  EXPECT_FALSE(isArray);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsArray_String) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  bool isArray = true;
  ASSERT_EQ(napi_ok, napi_is_array(env_, str, &isArray));
  EXPECT_FALSE(isArray);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsArray_Null) {
  napi_handle_scope scope = openScope(env_);

  napi_value nullVal = nullptr;
  ASSERT_EQ(napi_ok, napi_get_null(env_, &nullVal));

  bool isArray = true;
  ASSERT_EQ(napi_ok, napi_is_array(env_, nullVal, &isArray));
  EXPECT_FALSE(isArray);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsArray_Undefined) {
  napi_handle_scope scope = openScope(env_);

  napi_value undef = nullptr;
  ASSERT_EQ(napi_ok, napi_get_undefined(env_, &undef));

  bool isArray = true;
  ASSERT_EQ(napi_ok, napi_is_array(env_, undef, &isArray));
  EXPECT_FALSE(isArray);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsArray_NullEnv) {
  bool isArray = false;
  EXPECT_EQ(napi_invalid_arg, napi_is_array(nullptr, nullptr, &isArray));
}

TEST_F(NapiTestFixture, IsArray_NullValue) {
  napi_handle_scope scope = openScope(env_);

  bool isArray = false;
  EXPECT_EQ(napi_invalid_arg, napi_is_array(env_, nullptr, &isArray));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsArray_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value arr = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array(env_, &arr));

  EXPECT_EQ(napi_invalid_arg, napi_is_array(env_, arr, nullptr));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_array_length
//===========================================================================

TEST_F(NapiTestFixture, GetArrayLength_Empty) {
  napi_handle_scope scope = openScope(env_);

  napi_value arr = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array(env_, &arr));

  uint32_t length = 99;
  ASSERT_EQ(napi_ok, napi_get_array_length(env_, arr, &length));
  EXPECT_EQ(0u, length);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetArrayLength_PreAllocated) {
  napi_handle_scope scope = openScope(env_);

  napi_value arr = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array_with_length(env_, 100, &arr));

  uint32_t length = 0;
  ASSERT_EQ(napi_ok, napi_get_array_length(env_, arr, &length));
  EXPECT_EQ(100u, length);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetArrayLength_AfterSetElement) {
  // Setting elements on an array should update the length.
  napi_handle_scope scope = openScope(env_);

  napi_value arr = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array(env_, &arr));

  // Set element at index 4 — this should make length 5.
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &val));
  ASSERT_EQ(napi_ok, napi_set_element(env_, arr, 4, val));

  uint32_t length = 0;
  ASSERT_EQ(napi_ok, napi_get_array_length(env_, arr, &length));
  EXPECT_EQ(5u, length);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetArrayLength_NotAnArray) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  uint32_t length = 0;
  EXPECT_EQ(napi_array_expected, napi_get_array_length(env_, obj, &length));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetArrayLength_Number) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &num));

  uint32_t length = 0;
  EXPECT_EQ(napi_array_expected, napi_get_array_length(env_, num, &length));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetArrayLength_NullEnv) {
  uint32_t length = 0;
  EXPECT_EQ(napi_invalid_arg, napi_get_array_length(nullptr, nullptr, &length));
}

TEST_F(NapiTestFixture, GetArrayLength_NullValue) {
  napi_handle_scope scope = openScope(env_);

  uint32_t length = 0;
  EXPECT_EQ(napi_invalid_arg, napi_get_array_length(env_, nullptr, &length));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetArrayLength_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value arr = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array(env_, &arr));

  EXPECT_EQ(napi_invalid_arg, napi_get_array_length(env_, arr, nullptr));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetArrayLength_PendingException) {
  napi_handle_scope scope = openScope(env_);

  napi_value arr = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array(env_, &arr));

  // Set a pending exception.
  napi_value err = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "err", 3, &err));
  ASSERT_EQ(napi_ok, napi_throw(env_, err));

  // napi_get_array_length uses NAPI_PREAMBLE, so it should fail.
  uint32_t length = 0;
  EXPECT_EQ(napi_pending_exception, napi_get_array_length(env_, arr, &length));

  // Clear the exception.
  napi_value exc = nullptr;
  ASSERT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &exc));

  closeScope(env_, scope);
}

//===========================================================================
// Array element operations (array-specific behaviors)
//===========================================================================

TEST_F(NapiTestFixture, Array_SetAndGetElements) {
  // Test setting and getting elements on an actual array.
  napi_handle_scope scope = openScope(env_);

  napi_value arr = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array(env_, &arr));

  // Push elements by index.
  for (int i = 0; i < 5; ++i) {
    napi_value val = nullptr;
    ASSERT_EQ(napi_ok, napi_create_int32(env_, i * 100, &val));
    ASSERT_EQ(napi_ok, napi_set_element(env_, arr, i, val));
  }

  // Verify length.
  uint32_t length = 0;
  ASSERT_EQ(napi_ok, napi_get_array_length(env_, arr, &length));
  EXPECT_EQ(5u, length);

  // Read them back.
  for (int i = 0; i < 5; ++i) {
    napi_value result = nullptr;
    ASSERT_EQ(napi_ok, napi_get_element(env_, arr, i, &result));
    double d = 0;
    ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
    EXPECT_EQ(static_cast<double>(i * 100), d);
  }

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Array_IsNotPlainObject) {
  // Verify an array is an array but also reports as napi_object.
  napi_handle_scope scope = openScope(env_);

  napi_value arr = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array(env_, &arr));

  bool isArray = false;
  ASSERT_EQ(napi_ok, napi_is_array(env_, arr, &isArray));
  EXPECT_TRUE(isArray);

  // typeof should still return napi_object (arrays are objects).
  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, arr, &type));
  EXPECT_EQ(napi_object, type);

  // But a plain object should not be an array.
  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  isArray = true;
  ASSERT_EQ(napi_ok, napi_is_array(env_, obj, &isArray));
  EXPECT_FALSE(isArray);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Array_SparseElements) {
  // Test that setting a high index creates a sparse array with the
  // correct length.
  napi_handle_scope scope = openScope(env_);

  napi_value arr = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array(env_, &arr));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &val));
  ASSERT_EQ(napi_ok, napi_set_element(env_, arr, 999, val));

  uint32_t length = 0;
  ASSERT_EQ(napi_ok, napi_get_array_length(env_, arr, &length));
  EXPECT_EQ(1000u, length);

  // Element at index 0 should be undefined.
  napi_value elem = nullptr;
  ASSERT_EQ(napi_ok, napi_get_element(env_, arr, 0, &elem));
  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, elem, &type));
  EXPECT_EQ(napi_undefined, type);

  // Element at index 999 should be 42.
  napi_value elem999 = nullptr;
  ASSERT_EQ(napi_ok, napi_get_element(env_, arr, 999, &elem999));
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, elem999, &d));
  EXPECT_EQ(42.0, d);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Array_CreateArrayNotAffectedByPendingException) {
  // napi_create_array uses CHECK_ENV, not NAPI_PREAMBLE, so it should
  // succeed even with a pending exception (following V8 behavior).
  napi_handle_scope scope = openScope(env_);

  // Set a pending exception.
  napi_value err = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "err", 3, &err));
  ASSERT_EQ(napi_ok, napi_throw(env_, err));

  // Should still succeed.
  napi_value arr = nullptr;
  EXPECT_EQ(napi_ok, napi_create_array(env_, &arr));
  EXPECT_NE(nullptr, arr);

  // Clear the exception.
  napi_value exc = nullptr;
  ASSERT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &exc));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, Array_IsArrayNotAffectedByPendingException) {
  // napi_is_array uses CHECK_ENV, not NAPI_PREAMBLE.
  napi_handle_scope scope = openScope(env_);

  napi_value arr = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array(env_, &arr));

  // Set a pending exception.
  napi_value err = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "err", 3, &err));
  ASSERT_EQ(napi_ok, napi_throw(env_, err));

  // Should still succeed.
  bool isArray = false;
  EXPECT_EQ(napi_ok, napi_is_array(env_, arr, &isArray));
  EXPECT_TRUE(isArray);

  // Clear the exception.
  napi_value exc = nullptr;
  ASSERT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &exc));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_property_names
//===========================================================================

TEST_F(NapiTestFixture, GetPropertyNames_EmptyObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value names = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property_names(env_, obj, &names));

  // Should be an array.
  bool isArray = false;
  ASSERT_EQ(napi_ok, napi_is_array(env_, names, &isArray));
  EXPECT_TRUE(isArray);

  // Should be empty.
  uint32_t length = 99;
  ASSERT_EQ(napi_ok, napi_get_array_length(env_, names, &length));
  EXPECT_EQ(0u, length);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetPropertyNames_WithProperties) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Add three properties.
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));
  ASSERT_EQ(napi_ok, napi_set_named_property(env_, obj, "a", val));
  ASSERT_EQ(napi_ok, napi_set_named_property(env_, obj, "b", val));
  ASSERT_EQ(napi_ok, napi_set_named_property(env_, obj, "c", val));

  napi_value names = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property_names(env_, obj, &names));

  uint32_t length = 0;
  ASSERT_EQ(napi_ok, napi_get_array_length(env_, names, &length));
  EXPECT_EQ(3u, length);

  // Verify each name is a string.
  for (uint32_t i = 0; i < length; ++i) {
    napi_value elem = nullptr;
    ASSERT_EQ(napi_ok, napi_get_element(env_, names, i, &elem));
    napi_valuetype type;
    ASSERT_EQ(napi_ok, napi_typeof(env_, elem, &type));
    EXPECT_EQ(napi_string, type);
  }

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetPropertyNames_NumericKeysAsStrings) {
  // Numeric (index) property keys should be converted to strings.
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &val));
  ASSERT_EQ(napi_ok, napi_set_element(env_, obj, 0, val));
  ASSERT_EQ(napi_ok, napi_set_element(env_, obj, 1, val));

  napi_value names = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property_names(env_, obj, &names));

  uint32_t length = 0;
  ASSERT_EQ(napi_ok, napi_get_array_length(env_, names, &length));
  EXPECT_EQ(2u, length);

  // Keys should be strings "0" and "1".
  napi_value elem0 = nullptr;
  ASSERT_EQ(napi_ok, napi_get_element(env_, names, 0, &elem0));
  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, elem0, &type));
  EXPECT_EQ(napi_string, type);

  char buf[16];
  size_t len = 0;
  ASSERT_EQ(
      napi_ok, napi_get_value_string_utf8(env_, elem0, buf, sizeof(buf), &len));
  EXPECT_STREQ("0", buf);

  napi_value elem1 = nullptr;
  ASSERT_EQ(napi_ok, napi_get_element(env_, names, 1, &elem1));
  ASSERT_EQ(
      napi_ok, napi_get_value_string_utf8(env_, elem1, buf, sizeof(buf), &len));
  EXPECT_STREQ("1", buf);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetPropertyNames_MixedKeys) {
  // Object with both string and numeric keys.
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));

  // Numeric key.
  ASSERT_EQ(napi_ok, napi_set_element(env_, obj, 0, val));
  // String key.
  ASSERT_EQ(napi_ok, napi_set_named_property(env_, obj, "name", val));

  napi_value names = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property_names(env_, obj, &names));

  uint32_t length = 0;
  ASSERT_EQ(napi_ok, napi_get_array_length(env_, names, &length));
  EXPECT_EQ(2u, length);

  // ES2015 order: numeric keys first ("0"), then string keys ("name").
  napi_value elem0 = nullptr;
  ASSERT_EQ(napi_ok, napi_get_element(env_, names, 0, &elem0));
  char buf[16];
  size_t len = 0;
  ASSERT_EQ(
      napi_ok, napi_get_value_string_utf8(env_, elem0, buf, sizeof(buf), &len));
  EXPECT_STREQ("0", buf);

  napi_value elem1 = nullptr;
  ASSERT_EQ(napi_ok, napi_get_element(env_, names, 1, &elem1));
  ASSERT_EQ(
      napi_ok, napi_get_value_string_utf8(env_, elem1, buf, sizeof(buf), &len));
  EXPECT_STREQ("name", buf);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetPropertyNames_NullEnv) {
  EXPECT_EQ(
      napi_invalid_arg, napi_get_property_names(nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, GetPropertyNames_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  EXPECT_EQ(napi_invalid_arg, napi_get_property_names(env_, obj, nullptr));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetPropertyNames_NotAnObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &num));

  napi_value names = nullptr;
  EXPECT_EQ(napi_object_expected, napi_get_property_names(env_, num, &names));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetPropertyNames_PendingException) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Set a pending exception.
  napi_value err = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "err", 3, &err));
  ASSERT_EQ(napi_ok, napi_throw(env_, err));

  napi_value names = nullptr;
  EXPECT_EQ(napi_pending_exception, napi_get_property_names(env_, obj, &names));

  // Clear the exception.
  napi_value exc = nullptr;
  ASSERT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &exc));

  closeScope(env_, scope);
}

//===========================================================================
// napi_has_own_property
//===========================================================================

TEST_F(NapiTestFixture, HasOwnProperty_Exists) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));
  ASSERT_EQ(napi_ok, napi_set_named_property(env_, obj, "own", val));

  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "own", 3, &key));

  bool hasOwn = false;
  ASSERT_EQ(napi_ok, napi_has_own_property(env_, obj, key, &hasOwn));
  EXPECT_TRUE(hasOwn);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasOwnProperty_NotExists) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "missing", 7, &key));

  bool hasOwn = true;
  ASSERT_EQ(napi_ok, napi_has_own_property(env_, obj, key, &hasOwn));
  EXPECT_FALSE(hasOwn);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasOwnProperty_InheritedNotOwn) {
  // Properties inherited from the prototype should NOT be reported
  // as own properties.
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // "toString" is inherited from Object.prototype but not own.
  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "toString", 8, &key));

  // napi_has_property should find it (searches prototype chain).
  bool hasProp = false;
  ASSERT_EQ(napi_ok, napi_has_property(env_, obj, key, &hasProp));
  EXPECT_TRUE(hasProp);

  // napi_has_own_property should NOT find it (own only).
  bool hasOwn = true;
  ASSERT_EQ(napi_ok, napi_has_own_property(env_, obj, key, &hasOwn));
  EXPECT_FALSE(hasOwn);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasOwnProperty_NumericKeyNotAllowed) {
  // V8 requires the key to be a "name" (string or symbol).
  // Passing a number should return napi_name_expected.
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value numKey = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 0, &numKey));

  bool hasOwn = false;
  EXPECT_EQ(
      napi_name_expected, napi_has_own_property(env_, obj, numKey, &hasOwn));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasOwnProperty_NullEnv) {
  EXPECT_EQ(
      napi_invalid_arg,
      napi_has_own_property(nullptr, nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, HasOwnProperty_NullKey) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  bool hasOwn = false;
  EXPECT_EQ(
      napi_invalid_arg, napi_has_own_property(env_, obj, nullptr, &hasOwn));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasOwnProperty_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));
  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "k", 1, &key));

  EXPECT_EQ(napi_invalid_arg, napi_has_own_property(env_, obj, key, nullptr));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasOwnProperty_NotAnObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &num));
  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "k", 1, &key));

  bool hasOwn = false;
  EXPECT_EQ(
      napi_object_expected, napi_has_own_property(env_, num, key, &hasOwn));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, HasOwnProperty_PendingException) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));
  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "k", 1, &key));

  // Set a pending exception.
  napi_value err = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "err", 3, &err));
  ASSERT_EQ(napi_ok, napi_throw(env_, err));

  bool hasOwn = false;
  EXPECT_EQ(
      napi_pending_exception, napi_has_own_property(env_, obj, key, &hasOwn));

  // Clear the exception.
  napi_value exc = nullptr;
  ASSERT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &exc));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_prototype
//===========================================================================

TEST_F(NapiTestFixture, GetPrototype_PlainObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value proto = nullptr;
  ASSERT_EQ(napi_ok, napi_get_prototype(env_, obj, &proto));

  // The prototype of a plain object is Object.prototype, which is
  // an object (not null or undefined).
  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, proto, &type));
  EXPECT_EQ(napi_object, type);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetPrototype_ObjectPrototypeIsNotNull) {
  // Object.prototype's prototype is null (it's at the end of the
  // prototype chain).
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Get Object.prototype.
  napi_value proto1 = nullptr;
  ASSERT_EQ(napi_ok, napi_get_prototype(env_, obj, &proto1));

  // Get Object.prototype.__proto__ (should be null).
  napi_value proto2 = nullptr;
  ASSERT_EQ(napi_ok, napi_get_prototype(env_, proto1, &proto2));
  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, proto2, &type));
  EXPECT_EQ(napi_null, type);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetPrototype_Array) {
  // An array's prototype is Array.prototype, which is an object.
  napi_handle_scope scope = openScope(env_);

  napi_value arr = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array(env_, &arr));

  napi_value proto = nullptr;
  ASSERT_EQ(napi_ok, napi_get_prototype(env_, arr, &proto));

  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, proto, &type));
  EXPECT_EQ(napi_object, type);

  // Array.prototype is itself an object, not null.
  EXPECT_NE(napi_null, type);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetPrototype_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_get_prototype(nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, GetPrototype_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  EXPECT_EQ(napi_invalid_arg, napi_get_prototype(env_, obj, nullptr));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetPrototype_NotAnObject) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 42, &num));

  napi_value proto = nullptr;
  EXPECT_EQ(napi_object_expected, napi_get_prototype(env_, num, &proto));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetPrototype_PendingException) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Set a pending exception.
  napi_value err = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "err", 3, &err));
  ASSERT_EQ(napi_ok, napi_throw(env_, err));

  napi_value proto = nullptr;
  EXPECT_EQ(napi_pending_exception, napi_get_prototype(env_, obj, &proto));

  // Clear the exception.
  napi_value exc = nullptr;
  ASSERT_EQ(napi_ok, napi_get_and_clear_last_exception(env_, &exc));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetPrototype_PrototypeHasProperties) {
  // Verify that inherited properties from the prototype are accessible.
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value proto = nullptr;
  ASSERT_EQ(napi_ok, napi_get_prototype(env_, obj, &proto));

  // Object.prototype should have "toString" as its own property.
  napi_value key = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "toString", 8, &key));
  bool hasOwn = false;
  ASSERT_EQ(napi_ok, napi_has_own_property(env_, proto, key, &hasOwn));
  EXPECT_TRUE(hasOwn);

  closeScope(env_, scope);
}

//===========================================================================
// napi_define_properties
//===========================================================================

TEST_F(NapiTestFixture, DefineProperties_NullEnv) {
  EXPECT_EQ(
      napi_invalid_arg, napi_define_properties(nullptr, nullptr, 0, nullptr));
}

TEST_F(NapiTestFixture, DefineProperties_NotObject) {
  auto scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 42.0, &num));

  napi_property_descriptor props[] = {};
  EXPECT_EQ(napi_object_expected, napi_define_properties(env_, num, 0, props));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DefineProperties_NullProps) {
  auto scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // property_count > 0 but properties is null.
  EXPECT_EQ(napi_invalid_arg, napi_define_properties(env_, obj, 1, nullptr));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DefineProperties_ZeroCount) {
  auto scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Zero properties is a no-op.
  EXPECT_EQ(napi_ok, napi_define_properties(env_, obj, 0, nullptr));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DefineProperties_DataPropertyUtf8) {
  auto scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 42.0, &val));

  napi_property_descriptor props[] = {
      {"myProp",
       nullptr,
       nullptr,
       nullptr,
       nullptr,
       val,
       napi_default_jsproperty,
       nullptr},
  };
  EXPECT_EQ(napi_ok, napi_define_properties(env_, obj, 1, props));

  // Verify the property exists.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "myProp", &result));
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(42.0, d);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DefineProperties_DataPropertyNameValue) {
  auto scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value key = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_string_utf8(env_, "foo", NAPI_AUTO_LENGTH, &key));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 99.0, &val));

  napi_property_descriptor props[] = {
      {nullptr,
       key,
       nullptr,
       nullptr,
       nullptr,
       val,
       napi_default_jsproperty,
       nullptr},
  };
  EXPECT_EQ(napi_ok, napi_define_properties(env_, obj, 1, props));

  // Verify the property exists.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property(env_, obj, key, &result));
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(99.0, d);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DefineProperties_ReadOnly) {
  auto scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 10.0, &val));

  // Define a property without napi_writable (read-only).
  napi_property_descriptor props[] = {
      {"readOnly",
       nullptr,
       nullptr,
       nullptr,
       nullptr,
       val,
       napi_enumerable,
       nullptr},
  };
  EXPECT_EQ(napi_ok, napi_define_properties(env_, obj, 1, props));

  // Try to write a new value — should silently fail in non-strict mode.
  napi_value newVal = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 20.0, &newVal));
  ASSERT_EQ(napi_ok, napi_set_named_property(env_, obj, "readOnly", newVal));

  // Value should still be 10.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "readOnly", &result));
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(10.0, d);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DefineProperties_NotEnumerable) {
  auto scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 5.0, &val));

  // Define a property without napi_enumerable.
  napi_property_descriptor props[] = {
      {"hidden",
       nullptr,
       nullptr,
       nullptr,
       nullptr,
       val,
       static_cast<napi_property_attributes>(napi_writable | napi_configurable),
       nullptr},
  };
  EXPECT_EQ(napi_ok, napi_define_properties(env_, obj, 1, props));

  // The property should exist.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "hidden", &result));
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(5.0, d);

  // But it should NOT appear in property names (Object.keys).
  napi_value keys = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property_names(env_, obj, &keys));
  uint32_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_array_length(env_, keys, &len));
  EXPECT_EQ(0u, len);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DefineProperties_NotConfigurable) {
  auto scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 7.0, &val));

  // Define a non-configurable property.
  napi_property_descriptor props[] = {
      {"locked",
       nullptr,
       nullptr,
       nullptr,
       nullptr,
       val,
       static_cast<napi_property_attributes>(napi_writable | napi_enumerable),
       nullptr},
  };
  EXPECT_EQ(napi_ok, napi_define_properties(env_, obj, 1, props));

  // Try to delete the property — should fail because it's
  // non-configurable.
  napi_value key = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_string_utf8(env_, "locked", NAPI_AUTO_LENGTH, &key));
  bool deleted = true;
  ASSERT_EQ(napi_ok, napi_delete_property(env_, obj, key, &deleted));
  // The delete should return false (property was not deleted).
  EXPECT_FALSE(deleted);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DefineProperties_MethodUtf8) {
  auto scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Define a method that returns 100.
  napi_callback methodCb = [](napi_env env, napi_callback_info) -> napi_value {
    napi_value result = nullptr;
    napi_create_double(env, 100.0, &result);
    return result;
  };

  napi_property_descriptor props[] = {
      {"myMethod",
       nullptr,
       methodCb,
       nullptr,
       nullptr,
       nullptr,
       napi_default_method,
       nullptr},
  };
  EXPECT_EQ(napi_ok, napi_define_properties(env_, obj, 1, props));

  // Verify the property is a function.
  napi_value func = nullptr;
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "myMethod", &func));
  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, func, &type));
  EXPECT_EQ(napi_function, type);

  // Call it.
  auto *phv = reinterpret_cast<PinnedHermesValue *>(func);
  auto funcHandle = Handle<Callable>::vmcast(phv);
  {
    auto res = Callable::executeCall0(
        funcHandle, env_->runtime, Runtime::getUndefinedValue());
    ASSERT_NE(ExecutionStatus::EXCEPTION, res.getStatus());
    EXPECT_TRUE(res->get().isNumber());
    EXPECT_EQ(100.0, res->get().getNumber());
  }

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DefineProperties_MethodWithData) {
  auto scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  int userData = 555;

  // Define a method that returns its data pointer value.
  napi_callback methodCb = [](napi_env env,
                              napi_callback_info info) -> napi_value {
    void *data = nullptr;
    napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
    int val = *static_cast<int *>(data);
    napi_value result = nullptr;
    napi_create_double(env, val, &result);
    return result;
  };

  napi_property_descriptor props[] = {
      {"dataMethod",
       nullptr,
       methodCb,
       nullptr,
       nullptr,
       nullptr,
       napi_default_method,
       &userData},
  };
  EXPECT_EQ(napi_ok, napi_define_properties(env_, obj, 1, props));

  // Call it.
  napi_value func = nullptr;
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "dataMethod", &func));
  auto *phv = reinterpret_cast<PinnedHermesValue *>(func);
  auto funcHandle = Handle<Callable>::vmcast(phv);
  {
    auto res = Callable::executeCall0(
        funcHandle, env_->runtime, Runtime::getUndefinedValue());
    ASSERT_NE(ExecutionStatus::EXCEPTION, res.getStatus());
    EXPECT_EQ(555.0, res->get().getNumber());
  }

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DefineProperties_GetterOnly) {
  auto scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Define a getter that returns 42.
  napi_callback getter = [](napi_env env, napi_callback_info) -> napi_value {
    napi_value result = nullptr;
    napi_create_double(env, 42.0, &result);
    return result;
  };

  napi_property_descriptor props[] = {
      {"gProp",
       nullptr,
       nullptr,
       getter,
       nullptr,
       nullptr,
       static_cast<napi_property_attributes>(
           napi_enumerable | napi_configurable),
       nullptr},
  };
  EXPECT_EQ(napi_ok, napi_define_properties(env_, obj, 1, props));

  // Read the property — should invoke the getter.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "gProp", &result));
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(42.0, d);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DefineProperties_SetterOnly) {
  auto scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // We'll store the set value in a side-channel via data pointer.
  double stored = 0;

  napi_callback setter = [](napi_env env,
                            napi_callback_info info) -> napi_value {
    size_t argc = 1;
    napi_value argv[1];
    void *data = nullptr;
    napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
    double *ptr = static_cast<double *>(data);
    napi_get_value_double(env, argv[0], ptr);
    return nullptr;
  };

  napi_property_descriptor props[] = {
      {"sProp",
       nullptr,
       nullptr,
       nullptr,
       setter,
       nullptr,
       static_cast<napi_property_attributes>(
           napi_enumerable | napi_configurable),
       &stored},
  };
  EXPECT_EQ(napi_ok, napi_define_properties(env_, obj, 1, props));

  // Set the property — should invoke the setter.
  napi_value newVal = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 77.0, &newVal));
  ASSERT_EQ(napi_ok, napi_set_named_property(env_, obj, "sProp", newVal));

  EXPECT_EQ(77.0, stored);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DefineProperties_GetterAndSetter) {
  auto scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Shared storage for the accessor.
  double storage = 10.0;

  napi_callback getter = [](napi_env env,
                            napi_callback_info info) -> napi_value {
    void *data = nullptr;
    napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
    double val = *static_cast<double *>(data);
    napi_value result = nullptr;
    napi_create_double(env, val, &result);
    return result;
  };

  napi_callback setter = [](napi_env env,
                            napi_callback_info info) -> napi_value {
    size_t argc = 1;
    napi_value argv[1];
    void *data = nullptr;
    napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
    double *ptr = static_cast<double *>(data);
    napi_get_value_double(env, argv[0], ptr);
    return nullptr;
  };

  napi_property_descriptor props[] = {
      {"gsProp",
       nullptr,
       nullptr,
       getter,
       setter,
       nullptr,
       static_cast<napi_property_attributes>(
           napi_enumerable | napi_configurable),
       &storage},
  };
  EXPECT_EQ(napi_ok, napi_define_properties(env_, obj, 1, props));

  // Read — should invoke getter, return 10.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "gsProp", &result));
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(10.0, d);

  // Write — should invoke setter.
  napi_value newVal = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 25.0, &newVal));
  ASSERT_EQ(napi_ok, napi_set_named_property(env_, obj, "gsProp", newVal));
  EXPECT_EQ(25.0, storage);

  // Read again — should see updated value.
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "gsProp", &result));
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(25.0, d);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DefineProperties_MultipleProps) {
  auto scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val1 = nullptr, val2 = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 1.0, &val1));
  ASSERT_EQ(napi_ok, napi_create_double(env_, 2.0, &val2));

  napi_callback methodCb = [](napi_env env, napi_callback_info) -> napi_value {
    napi_value result = nullptr;
    napi_create_double(env, 3.0, &result);
    return result;
  };

  napi_property_descriptor props[] = {
      {"a",
       nullptr,
       nullptr,
       nullptr,
       nullptr,
       val1,
       napi_default_jsproperty,
       nullptr},
      {"b",
       nullptr,
       nullptr,
       nullptr,
       nullptr,
       val2,
       napi_default_jsproperty,
       nullptr},
      {"c",
       nullptr,
       methodCb,
       nullptr,
       nullptr,
       nullptr,
       napi_default_method,
       nullptr},
  };
  EXPECT_EQ(napi_ok, napi_define_properties(env_, obj, 3, props));

  // Verify all three exist.
  napi_value result = nullptr;
  double d = 0;

  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "a", &result));
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(1.0, d);

  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "b", &result));
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(2.0, d);

  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "c", &result));
  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, result, &type));
  EXPECT_EQ(napi_function, type);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DefineProperties_SymbolKey) {
  auto scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Create a symbol via the VM API.
  napi_value symKey = nullptr;
  {
    auto strRes =
        StringPrimitive::createEfficient(env_->runtime, ASCIIRef{"sym", 3});
    ASSERT_NE(ExecutionStatus::EXCEPTION, strRes.getStatus());
    struct : public Locals {
      PinnedValue<StringPrimitive> str;
    } lv;
    LocalsRAII lraii(env_->runtime, &lv);
    lv.str = vmcast<StringPrimitive>(*strRes);
    Handle<StringPrimitive> strHandle = lv.str;
    auto sym = env_->runtime.getIdentifierTable().createNotUniquedSymbol(
        env_->runtime, strHandle);
    ASSERT_NE(ExecutionStatus::EXCEPTION, sym.getStatus());
    symKey = env_->addToCurrentScope(HermesValue::encodeSymbolValue(*sym));
  }

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 88.0, &val));

  napi_property_descriptor props[] = {
      {nullptr,
       symKey,
       nullptr,
       nullptr,
       nullptr,
       val,
       napi_default_jsproperty,
       nullptr},
  };
  EXPECT_EQ(napi_ok, napi_define_properties(env_, obj, 1, props));

  // Verify the property is accessible via the symbol key.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property(env_, obj, symKey, &result));
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(88.0, d);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DefineProperties_InvalidNameKey) {
  auto scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Using a number as the name key should fail.
  napi_value numKey = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 42.0, &numKey));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 1.0, &val));

  napi_property_descriptor props[] = {
      {nullptr,
       numKey,
       nullptr,
       nullptr,
       nullptr,
       val,
       napi_default_jsproperty,
       nullptr},
  };
  EXPECT_EQ(napi_name_expected, napi_define_properties(env_, obj, 1, props));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DefineProperties_PendingExceptionBlocks) {
  auto scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Set a pending exception.
  napi_value errMsg = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_create_string_utf8(env_, "test", NAPI_AUTO_LENGTH, &errMsg));
  napi_throw(env_, errMsg);

  // napi_define_properties should fail because of the pending
  // exception.
  EXPECT_EQ(
      napi_pending_exception, napi_define_properties(env_, obj, 0, nullptr));

  // Clear exception.
  napi_value exc = nullptr;
  napi_get_and_clear_last_exception(env_, &exc);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, DefineProperties_AccessorNotEnumerable) {
  auto scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_callback getter = [](napi_env env, napi_callback_info) -> napi_value {
    napi_value result = nullptr;
    napi_create_double(env, 1.0, &result);
    return result;
  };

  // Define a non-enumerable accessor.
  napi_property_descriptor props[] = {
      {"hidden",
       nullptr,
       nullptr,
       getter,
       nullptr,
       nullptr,
       napi_configurable,
       nullptr},
  };
  EXPECT_EQ(napi_ok, napi_define_properties(env_, obj, 1, props));

  // Property should be accessible.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, obj, "hidden", &result));
  double d = 0;
  ASSERT_EQ(napi_ok, napi_get_value_double(env_, result, &d));
  EXPECT_EQ(1.0, d);

  // But should NOT appear in property names.
  napi_value keys = nullptr;
  ASSERT_EQ(napi_ok, napi_get_property_names(env_, obj, &keys));
  uint32_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_array_length(env_, keys, &len));
  EXPECT_EQ(0u, len);

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_all_property_names
//===========================================================================

TEST_F(NapiTestFixture, GetAllPropertyNames_MixedStringAndSymbol) {
  // Regression test: when both string and symbol properties are requested
  // (i.e., neither skip_strings nor skip_symbols), string property names
  // must come back as JS strings, not as JS Symbols.
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Add a string property "foo".
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));
  ASSERT_EQ(napi_ok, napi_set_named_property(env_, obj, "foo", val));

  // Add a symbol property.
  napi_value sym = nullptr;
  ASSERT_EQ(napi_ok, napi_create_symbol(env_, /*description*/ nullptr, &sym));
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 2, &val));
  ASSERT_EQ(napi_ok, napi_set_property(env_, obj, sym, val));

  // Enumerate ALL properties (no skip flags) with napi_key_all_properties.
  napi_value names = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_get_all_property_names(
          env_,
          obj,
          napi_key_own_only,
          napi_key_all_properties,
          napi_key_keep_numbers,
          &names));

  uint32_t length = 0;
  ASSERT_EQ(napi_ok, napi_get_array_length(env_, names, &length));
  EXPECT_EQ(2u, length);

  // Check that "foo" comes back as a string, not a symbol.
  bool foundString = false;
  bool foundSymbol = false;
  for (uint32_t i = 0; i < length; ++i) {
    napi_value elem = nullptr;
    ASSERT_EQ(napi_ok, napi_get_element(env_, names, i, &elem));
    napi_valuetype type;
    ASSERT_EQ(napi_ok, napi_typeof(env_, elem, &type));
    if (type == napi_string) {
      foundString = true;
      // Verify the string content is "foo".
      size_t len = 0;
      ASSERT_EQ(
          napi_ok, napi_get_value_string_utf8(env_, elem, nullptr, 0, &len));
      std::string str(len, '\0');
      ASSERT_EQ(
          napi_ok,
          napi_get_value_string_utf8(env_, elem, &str[0], len + 1, &len));
      EXPECT_EQ("foo", str);
    } else if (type == napi_symbol) {
      foundSymbol = true;
    }
  }
  EXPECT_TRUE(foundString) << "String property 'foo' should have type "
                              "napi_string, not napi_symbol";
  EXPECT_TRUE(foundSymbol) << "Symbol property should be present";

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetAllPropertyNames_SkipStrings_OnlySymbols) {
  // When skip_strings is set, only symbol properties should be returned.
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  // Add string and symbol properties.
  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));
  ASSERT_EQ(napi_ok, napi_set_named_property(env_, obj, "bar", val));

  napi_value sym = nullptr;
  ASSERT_EQ(napi_ok, napi_create_symbol(env_, nullptr, &sym));
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 2, &val));
  ASSERT_EQ(napi_ok, napi_set_property(env_, obj, sym, val));

  // Enumerate with skip_strings.
  napi_value names = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_get_all_property_names(
          env_,
          obj,
          napi_key_own_only,
          napi_key_skip_strings,
          napi_key_keep_numbers,
          &names));

  uint32_t length = 0;
  ASSERT_EQ(napi_ok, napi_get_array_length(env_, names, &length));
  EXPECT_EQ(1u, length);

  // The single result should be a symbol.
  if (length > 0) {
    napi_value elem = nullptr;
    ASSERT_EQ(napi_ok, napi_get_element(env_, names, 0, &elem));
    napi_valuetype type;
    ASSERT_EQ(napi_ok, napi_typeof(env_, elem, &type));
    EXPECT_EQ(napi_symbol, type);
  }

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetAllPropertyNames_SkipSymbols_OnlyStrings) {
  // When skip_symbols is set, only string properties should be returned
  // and they should have type napi_string.
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value val = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &val));
  ASSERT_EQ(napi_ok, napi_set_named_property(env_, obj, "baz", val));

  napi_value sym = nullptr;
  ASSERT_EQ(napi_ok, napi_create_symbol(env_, nullptr, &sym));
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 2, &val));
  ASSERT_EQ(napi_ok, napi_set_property(env_, obj, sym, val));

  // Enumerate with skip_symbols.
  napi_value names = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_get_all_property_names(
          env_,
          obj,
          napi_key_own_only,
          napi_key_skip_symbols,
          napi_key_keep_numbers,
          &names));

  uint32_t length = 0;
  ASSERT_EQ(napi_ok, napi_get_array_length(env_, names, &length));
  EXPECT_EQ(1u, length);

  if (length > 0) {
    napi_value elem = nullptr;
    ASSERT_EQ(napi_ok, napi_get_element(env_, names, 0, &elem));
    napi_valuetype type;
    ASSERT_EQ(napi_ok, napi_typeof(env_, elem, &type));
    EXPECT_EQ(napi_string, type);
  }

  closeScope(env_, scope);
}

/// A (symbol value, display name) pair used by describeKeys().
using NamedSymbol = std::pair<napi_value, std::string>;

/// Render each element of the key array \p names as a string: numbers as
/// "#<n>", strings as themselves, and symbols as the name registered for them
/// in \p syms (or "<unknown symbol>").
static std::vector<std::string> describeKeys(
    napi_env env,
    napi_value names,
    const std::vector<NamedSymbol> &syms = {}) {
  std::vector<std::string> res;
  uint32_t length = 0;
  EXPECT_EQ(napi_ok, napi_get_array_length(env, names, &length));
  res.reserve(length);
  for (uint32_t i = 0; i < length; ++i) {
    napi_handle_scope scope = openScope(env);
    napi_value elem = nullptr;
    EXPECT_EQ(napi_ok, napi_get_element(env, names, i, &elem));
    napi_valuetype type;
    EXPECT_EQ(napi_ok, napi_typeof(env, elem, &type));
    if (type == napi_number) {
      double d = 0;
      EXPECT_EQ(napi_ok, napi_get_value_double(env, elem, &d));
      res.push_back("#" + std::to_string(static_cast<uint32_t>(d)));
    } else if (type == napi_string) {
      size_t len = 0;
      EXPECT_EQ(
          napi_ok, napi_get_value_string_utf8(env, elem, nullptr, 0, &len));
      std::string str(len, '\0');
      EXPECT_EQ(
          napi_ok,
          napi_get_value_string_utf8(env, elem, &str[0], len + 1, &len));
      res.push_back(std::move(str));
    } else if (type == napi_symbol) {
      std::string name = "<unknown symbol>";
      for (const auto &sym : syms) {
        bool eq = false;
        EXPECT_EQ(napi_ok, napi_strict_equals(env, elem, sym.first, &eq));
        if (eq) {
          name = sym.second;
          break;
        }
      }
      res.push_back(std::move(name));
    } else {
      res.push_back("<unexpected type>");
    }
    closeScope(env, scope);
  }
  return res;
}

/// Join \p v into a single space-separated string, for readable diffs.
static std::string join(const std::vector<std::string> &v) {
  std::string res;
  for (const std::string &e : v)
    res += (res.empty() ? "" : " ") + e;
  return res;
}

/// Set the own property \p name of \p obj to 1.
static void setNamed(napi_env env, napi_value obj, const char *name) {
  napi_value one = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env, 1, &one));
  ASSERT_EQ(napi_ok, napi_set_named_property(env, obj, name, one));
}

/// Set the own property keyed by symbol \p sym of \p obj to 1.
static void setSym(napi_env env, napi_value obj, napi_value sym) {
  napi_value one = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env, 1, &one));
  ASSERT_EQ(napi_ok, napi_set_property(env, obj, sym, one));
}

/// Set \p proto as the prototype of \p obj.
static void setProto(napi_env env, napi_value obj, napi_value proto) {
  napi_value global = nullptr;
  napi_value objectCtor = nullptr;
  napi_value setPrototypeOf = nullptr;
  napi_value args[2] = {obj, proto};
  napi_value ignored = nullptr;
  ASSERT_EQ(napi_ok, napi_get_global(env, &global));
  ASSERT_EQ(
      napi_ok, napi_get_named_property(env, global, "Object", &objectCtor));
  ASSERT_EQ(
      napi_ok,
      napi_get_named_property(
          env, objectCtor, "setPrototypeOf", &setPrototypeOf));
  ASSERT_EQ(
      napi_ok,
      napi_call_function(env, objectCtor, setPrototypeOf, 2, args, &ignored));
}

/// Make \p obj a null-prototype object, so that Object.prototype does not
/// contribute keys to a prototype-chain walk.
static void setProtoNull(napi_env env, napi_value obj) {
  napi_value null = nullptr;
  ASSERT_EQ(napi_ok, napi_get_null(env, &null));
  setProto(env, obj, null);
}

TEST_F(NapiTestFixture, GetAllPropertyNames_DedupAcrossPrototypes) {
  // Names that appear on both an object and its prototype are reported once,
  // for every combination of key conversion and symbol filter.
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  napi_value proto = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));
  ASSERT_EQ(napi_ok, napi_create_object(env_, &proto));
  setProto(env_, obj, proto);
  setProtoNull(env_, proto);

  napi_value sA = nullptr;
  napi_value sB = nullptr;
  ASSERT_EQ(napi_ok, napi_create_symbol(env_, nullptr, &sA));
  ASSERT_EQ(napi_ok, napi_create_symbol(env_, nullptr, &sB));
  std::vector<NamedSymbol> syms = {{sA, "symA"}, {sB, "symB"}};

  // Own: 5, "a", "b", "c", symA.   Proto: 5, "b", "c", "d", symA, symB.
  napi_value one = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &one));
  for (napi_value o : {obj, proto})
    ASSERT_EQ(napi_ok, napi_set_element(env_, o, 5, one));
  for (const char *n : {"a", "b", "c"})
    setNamed(env_, obj, n);
  for (const char *n : {"b", "c", "d"})
    setNamed(env_, proto, n);
  setSym(env_, obj, sA);
  setSym(env_, proto, sA);
  setSym(env_, proto, sB);

  auto run = [&](napi_key_filter filter, napi_key_conversion conv) {
    napi_value names = nullptr;
    EXPECT_EQ(
        napi_ok,
        napi_get_all_property_names(
            env_, obj, napi_key_include_prototypes, filter, conv, &names));
    return describeKeys(env_, names, syms);
  };

  using V = std::vector<std::string>;
  EXPECT_EQ(
      (V{"#5", "a", "b", "c", "symA", "d", "symB"}),
      run(napi_key_all_properties, napi_key_keep_numbers));
  EXPECT_EQ(
      (V{"5", "a", "b", "c", "symA", "d", "symB"}),
      run(napi_key_all_properties, napi_key_numbers_to_strings));
  EXPECT_EQ(
      (V{"#5", "a", "b", "c", "d"}),
      run(napi_key_skip_symbols, napi_key_keep_numbers));
  EXPECT_EQ(
      (V{"5", "a", "b", "c", "d"}),
      run(napi_key_skip_symbols, napi_key_numbers_to_strings));
  EXPECT_EQ(
      (V{"symA", "symB"}), run(napi_key_skip_strings, napi_key_keep_numbers));
  // The same, filtering on attributes (goes through the descriptor lookup).
  EXPECT_EQ(
      (V{"#5", "a", "b", "c", "symA", "d", "symB"}),
      run(napi_key_enumerable, napi_key_keep_numbers));
  EXPECT_EQ(
      (V{"#5", "a", "b", "c", "d"}),
      run(static_cast<napi_key_filter>(
              napi_key_enumerable | napi_key_skip_symbols),
          napi_key_keep_numbers));

  closeScope(env_, scope);
}

/// Like NapiTestFixture, but with room for large key arrays.
class NapiLargeHeapTest : public NapiTestFixture {
 protected:
  void SetUp() override {
    auto config = hermes::vm::RuntimeConfig::Builder()
                      .withGCConfig(
                          hermes::vm::GCConfig::Builder()
                              .withInitHeapSize(1 << 16)
                              .withMaxHeapSize(1 << 26)
                              .build())
                      .build();
    rt_ = hermes::vm::Runtime::create(config);
    env_ = hermes_napi_create_env(&*rt_);
  }
};

TEST_F(NapiLargeHeapTest, GetAllPropertyNames_NoIndexSymbolCollision) {
  // Regression test: the prototype-chain dedup used to encode an index n as
  // (n << 1) | 1 and a symbol as its raw SymbolID, so an odd SymbolID could
  // collide with an index and a distinct key was dropped. Fresh names get
  // consecutive SymbolIDs above the identifier table's current size, so a
  // large index range is guaranteed to overlap (SymbolID - 1) / 2 for the odd
  // ones.
  constexpr uint32_t kNumIndices = 20000;
  constexpr uint32_t kNumNames = 300;
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  napi_value proto = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));
  ASSERT_EQ(napi_ok, napi_create_object(env_, &proto));
  setProto(env_, obj, proto);
  setProtoNull(env_, proto);

  napi_value one = nullptr;
  ASSERT_EQ(napi_ok, napi_create_int32(env_, 1, &one));
  for (uint32_t i = 0; i < kNumIndices; ++i)
    ASSERT_EQ(napi_ok, napi_set_element(env_, obj, i, one));
  for (uint32_t i = 0; i < kNumNames; ++i)
    setNamed(env_, proto, ("p" + std::to_string(i)).c_str());

  napi_value names = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_get_all_property_names(
          env_,
          obj,
          napi_key_include_prototypes,
          napi_key_all_properties,
          napi_key_keep_numbers,
          &names));

  std::vector<std::string> keys = describeKeys(env_, names);
  ASSERT_EQ(kNumIndices + kNumNames, keys.size());
  for (uint32_t i = 0; i < kNumIndices; ++i)
    ASSERT_EQ("#" + std::to_string(i), keys[i]);
  std::set<std::string> seen(keys.begin() + kNumIndices, keys.end());
  EXPECT_EQ(kNumNames, seen.size());
  for (uint32_t i = 0; i < kNumNames; ++i)
    EXPECT_EQ(1u, seen.count("p" + std::to_string(i))) << "p" << i;

  closeScope(env_, scope);
}

/// Native function that forces a full garbage collection.
static napi_value forceGC(napi_env env, napi_callback_info) {
  env->runtime.collect("napi test");
  return nullptr;
}

TEST_F(NapiLargeHeapTest, GetAllPropertyNames_ProxyWithGC) {
  // Proxy traps run during the walk and force collections (which move
  // objects), while the dedup bookkeeping holds keys across them. Trap results
  // are fresh, non-identifier-table strings.
  napi_handle_scope scope = openScope(env_);

  napi_value gcFn = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_create_function(
          env_, "gc", NAPI_AUTO_LENGTH, forceGC, nullptr, &gcFn));

  static const char kScript[] = R"JS(
    (function(gc) {
      var S = Symbol('S'), S2 = Symbol('S2');
      var protoObj = Object.create(null);
      protoObj.k3 = 1; protoObj.t2 = 1; protoObj.z1 = 1; protoObj.z2 = 1;
      protoObj[S] = 1; protoObj[S2] = 1;
      var mid = Object.create(protoObj);
      mid.k7 = 1; mid.t1 = 1; mid.t2 = 1;
      var junk;
      function churn(tag) {
        junk = [];
        for (var i = 0; i < 200; i++) junk.push({a: tag + i, b: [i]});
        gc();
        junk = [];
      }
      var handler = {
        ownKeys: function(t) {
          var keys = [];
          for (var i = 0; i < 50; i++) { keys.push('k' + i); churn('own' + i); }
          keys.push(S);
          return keys;
        },
        getOwnPropertyDescriptor: function(t, k) {
          churn('d');
          return {value: 1, enumerable: true, configurable: true};
        },
        getPrototypeOf: function(t) { churn('p'); return mid; },
      };
      return {proxy: new Proxy({}, handler), S: S, S2: S2};
    })
  )JS";
  napi_value script = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_create_string_utf8(env_, kScript, NAPI_AUTO_LENGTH, &script));
  napi_value factory = nullptr;
  ASSERT_EQ(napi_ok, napi_run_script(env_, script, &factory));
  napi_value undef = nullptr;
  ASSERT_EQ(napi_ok, napi_get_undefined(env_, &undef));
  napi_value bundle = nullptr;
  ASSERT_EQ(
      napi_ok, napi_call_function(env_, undef, factory, 1, &gcFn, &bundle));

  napi_value proxy = nullptr;
  napi_value symS = nullptr;
  napi_value symS2 = nullptr;
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, bundle, "proxy", &proxy));
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, bundle, "S", &symS));
  ASSERT_EQ(napi_ok, napi_get_named_property(env_, bundle, "S2", &symS2));
  std::vector<NamedSymbol> syms = {{symS, "S"}, {symS2, "S2"}};

  std::vector<std::string> kNames;
  for (int i = 0; i < 50; ++i)
    kNames.push_back("k" + std::to_string(i));

  auto run = [&](napi_key_filter filter) {
    napi_value names = nullptr;
    EXPECT_EQ(
        napi_ok,
        napi_get_all_property_names(
            env_,
            proxy,
            napi_key_include_prototypes,
            filter,
            napi_key_keep_numbers,
            &names));
    return describeKeys(env_, names, syms);
  };

  std::vector<std::string> strs = kNames;
  strs.insert(strs.end(), {"t1", "t2", "z1", "z2"});
  std::vector<std::string> all = kNames;
  all.insert(all.end(), {"S", "t1", "t2", "z1", "z2", "S2"});

  EXPECT_EQ(join(all), join(run(napi_key_all_properties)));
  EXPECT_EQ(join(all), join(run(napi_key_enumerable)));
  EXPECT_EQ(join(strs), join(run(napi_key_skip_symbols)));
  EXPECT_EQ(
      join(strs),
      join(
          run(static_cast<napi_key_filter>(
              napi_key_enumerable | napi_key_skip_symbols))));
  EXPECT_EQ("S S2", join(run(napi_key_skip_strings)));
  EXPECT_EQ(
      "S S2",
      join(
          run(static_cast<napi_key_filter>(
              napi_key_configurable | napi_key_skip_strings))));

  closeScope(env_, scope);
}

TEST_F(NapiLargeHeapTest, GetAllPropertyNames_RecordedSymbolsStayAlive) {
  // The receiver's ownKeys trap returns fresh names, so only the dedup
  // bookkeeping keeps their SymbolIDs alive. The getPrototypeOf trap then
  // collects and returns a prototype with other fresh names. If a recorded
  // SymbolID were freed and reused for one of those, that name would be
  // dropped as a duplicate.
  napi_handle_scope scope = openScope(env_);

  napi_value gcFn = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_create_function(
          env_, "gc", NAPI_AUTO_LENGTH, forceGC, nullptr, &gcFn));

  static const char kScript[] = R"JS(
    (function(gc) {
      var desc = {value: 1, enumerable: true, configurable: true};
      return new Proxy(Object.create(null), {
        ownKeys: function(t) {
          var keys = [];
          for (var i = 0; i < 20; i++) keys.push('own' + i + '_' + i);
          return keys;
        },
        getOwnPropertyDescriptor: function(t, k) { return desc; },
        getPrototypeOf: function(t) {
          gc();
          var proto = Object.create(null);
          for (var i = 0; i < 20; i++) proto['other' + i + '_' + i] = 1;
          return proto;
        },
      });
    })
  )JS";
  napi_value script = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_create_string_utf8(env_, kScript, NAPI_AUTO_LENGTH, &script));
  napi_value factory = nullptr;
  ASSERT_EQ(napi_ok, napi_run_script(env_, script, &factory));
  napi_value undef = nullptr;
  ASSERT_EQ(napi_ok, napi_get_undefined(env_, &undef));
  napi_value proxy = nullptr;
  ASSERT_EQ(
      napi_ok, napi_call_function(env_, undef, factory, 1, &gcFn, &proxy));

  napi_value names = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_get_all_property_names(
          env_,
          proxy,
          napi_key_include_prototypes,
          napi_key_all_properties,
          napi_key_keep_numbers,
          &names));

  std::vector<std::string> expected;
  for (int i = 0; i < 20; ++i)
    expected.push_back("own" + std::to_string(i) + "_" + std::to_string(i));
  for (int i = 0; i < 20; ++i)
    expected.push_back("other" + std::to_string(i) + "_" + std::to_string(i));
  EXPECT_EQ(join(expected), join(describeKeys(env_, names)));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetAllPropertyNames_IndexLikeProxyKey) {
  // A Proxy ownKeys trap reports an index-like name as a string, while an
  // ordinary object reports it as a number. Dedup must treat them as the
  // same name, in either order along the prototype chain.
  napi_handle_scope scope = openScope(env_);

  static const char kScript[] = R"JS(
    (function() {
      var plain = Object.create(null);
      plain[0] = 1;
      plain.y = 1;
      var handler = {
        ownKeys: function(t) { return ['0', 'x']; },
        getOwnPropertyDescriptor: function(t, k) {
          return {value: 1, enumerable: true, configurable: true};
        },
      };
      // Proxy above an ordinary object that owns index 0.
      var below = Object.create(new Proxy(Object.create(null), handler));
      below[0] = 1;
      // Ordinary object that owns index 0 above a Proxy.
      var above = new Proxy(Object.create(null), {
        ownKeys: handler.ownKeys,
        getOwnPropertyDescriptor: handler.getOwnPropertyDescriptor,
        getPrototypeOf: function(t) { return plain; },
      });
      return {below: below, above: above};
    })()
  )JS";
  napi_value script = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_create_string_utf8(env_, kScript, NAPI_AUTO_LENGTH, &script));
  napi_value bundle = nullptr;
  ASSERT_EQ(napi_ok, napi_run_script(env_, script, &bundle));

  auto run = [&](const char *name) {
    napi_value obj = nullptr;
    EXPECT_EQ(napi_ok, napi_get_named_property(env_, bundle, name, &obj));
    napi_value names = nullptr;
    EXPECT_EQ(
        napi_ok,
        napi_get_all_property_names(
            env_,
            obj,
            napi_key_include_prototypes,
            napi_key_all_properties,
            napi_key_keep_numbers,
            &names));
    return join(describeKeys(env_, names));
  };

  EXPECT_EQ("#0 x", run("below"));
  EXPECT_EQ("0 x y", run("above"));

  closeScope(env_, scope);
}

} // namespace

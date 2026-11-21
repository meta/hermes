/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NapiTestFixture.h"

#include <cstring>

namespace {

using hermes::napi::NapiTestFixture;

/// Helper to open a handle scope and return the scope handle.
static napi_handle_scope openScope(napi_env env) {
  napi_handle_scope scope = nullptr;
  EXPECT_EQ(napi_ok, napi_open_handle_scope(env, &scope));
  return scope;
}

/// Helper to close a handle scope.
static void closeScope(napi_env env, napi_handle_scope scope) {
  EXPECT_EQ(napi_ok, napi_close_handle_scope(env, scope));
}

//===========================================================================
// napi_create_arraybuffer - argument validation
//===========================================================================

TEST_F(NapiTestFixture, CreateArrayBuffer_NullEnv) {
  EXPECT_EQ(
      napi_invalid_arg, napi_create_arraybuffer(nullptr, 0, nullptr, nullptr));
}

TEST_F(NapiTestFixture, CreateArrayBuffer_NullResult) {
  napi_handle_scope scope = openScope(env_);
  EXPECT_EQ(
      napi_invalid_arg, napi_create_arraybuffer(env_, 0, nullptr, nullptr));
  closeScope(env_, scope);
}

//===========================================================================
// napi_create_arraybuffer - basic creation
//===========================================================================

TEST_F(NapiTestFixture, CreateArrayBuffer_ZeroLength) {
  napi_handle_scope scope = openScope(env_);

  void *data = nullptr;
  napi_value ab = nullptr;
  ASSERT_EQ(napi_ok, napi_create_arraybuffer(env_, 0, &data, &ab));
  ASSERT_NE(nullptr, ab);

  // Verify it is an ArrayBuffer.
  bool isAB = false;
  ASSERT_EQ(napi_ok, napi_is_arraybuffer(env_, ab, &isAB));
  EXPECT_TRUE(isAB);

  // Verify typeof returns napi_object (ArrayBuffer is an object).
  napi_valuetype type = napi_undefined;
  ASSERT_EQ(napi_ok, napi_typeof(env_, ab, &type));
  EXPECT_EQ(napi_object, type);

  // Verify info.
  void *infoData = nullptr;
  size_t byteLen = 99;
  ASSERT_EQ(napi_ok, napi_get_arraybuffer_info(env_, ab, &infoData, &byteLen));
  EXPECT_EQ(0u, byteLen);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateArrayBuffer_NonZeroLength) {
  napi_handle_scope scope = openScope(env_);

  void *data = nullptr;
  napi_value ab = nullptr;
  ASSERT_EQ(napi_ok, napi_create_arraybuffer(env_, 16, &data, &ab));
  ASSERT_NE(nullptr, ab);
  ASSERT_NE(nullptr, data);

  // Verify it is an ArrayBuffer.
  bool isAB = false;
  ASSERT_EQ(napi_ok, napi_is_arraybuffer(env_, ab, &isAB));
  EXPECT_TRUE(isAB);

  // Verify info returns the same data pointer and correct length.
  void *infoData = nullptr;
  size_t byteLen = 0;
  ASSERT_EQ(napi_ok, napi_get_arraybuffer_info(env_, ab, &infoData, &byteLen));
  EXPECT_EQ(data, infoData);
  EXPECT_EQ(16u, byteLen);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateArrayBuffer_WriteAndRead) {
  napi_handle_scope scope = openScope(env_);

  void *data = nullptr;
  napi_value ab = nullptr;
  ASSERT_EQ(napi_ok, napi_create_arraybuffer(env_, 8, &data, &ab));
  ASSERT_NE(nullptr, data);

  // Write data via the pointer.
  auto *bytes = static_cast<uint8_t *>(data);
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<uint8_t>(i + 1);
  }

  // Read back via info to verify the pointer is the same buffer.
  void *infoData = nullptr;
  size_t byteLen = 0;
  ASSERT_EQ(napi_ok, napi_get_arraybuffer_info(env_, ab, &infoData, &byteLen));
  EXPECT_EQ(8u, byteLen);

  auto *readBytes = static_cast<uint8_t *>(infoData);
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(i + 1), readBytes[i]);
  }

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateArrayBuffer_NullDataPtr) {
  // data can be null — the caller doesn't need the pointer.
  napi_handle_scope scope = openScope(env_);

  napi_value ab = nullptr;
  ASSERT_EQ(napi_ok, napi_create_arraybuffer(env_, 32, nullptr, &ab));
  ASSERT_NE(nullptr, ab);

  // Verify info still works.
  void *infoData = nullptr;
  size_t byteLen = 0;
  ASSERT_EQ(napi_ok, napi_get_arraybuffer_info(env_, ab, &infoData, &byteLen));
  EXPECT_NE(nullptr, infoData);
  EXPECT_EQ(32u, byteLen);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateArrayBuffer_ZeroInitialized) {
  // createDataBlock uses zero=true by default, so data should be zeroed.
  napi_handle_scope scope = openScope(env_);

  void *data = nullptr;
  napi_value ab = nullptr;
  ASSERT_EQ(napi_ok, napi_create_arraybuffer(env_, 16, &data, &ab));
  ASSERT_NE(nullptr, data);

  auto *bytes = static_cast<uint8_t *>(data);
  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(0, bytes[i]);
  }

  closeScope(env_, scope);
}

//===========================================================================
// napi_is_arraybuffer - argument validation
//===========================================================================

TEST_F(NapiTestFixture, IsArrayBuffer_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_is_arraybuffer(nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, IsArrayBuffer_NullValue) {
  bool result = false;
  EXPECT_EQ(napi_invalid_arg, napi_is_arraybuffer(env_, nullptr, &result));
}

TEST_F(NapiTestFixture, IsArrayBuffer_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value ab = nullptr;
  ASSERT_EQ(napi_ok, napi_create_arraybuffer(env_, 0, nullptr, &ab));
  EXPECT_EQ(napi_invalid_arg, napi_is_arraybuffer(env_, ab, nullptr));

  closeScope(env_, scope);
}

//===========================================================================
// napi_is_arraybuffer - type discrimination
//===========================================================================

TEST_F(NapiTestFixture, IsArrayBuffer_ArrayBufferIsTrue) {
  napi_handle_scope scope = openScope(env_);

  napi_value ab = nullptr;
  ASSERT_EQ(napi_ok, napi_create_arraybuffer(env_, 16, nullptr, &ab));

  bool isAB = false;
  ASSERT_EQ(napi_ok, napi_is_arraybuffer(env_, ab, &isAB));
  EXPECT_TRUE(isAB);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsArrayBuffer_ObjectIsFalse) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  bool isAB = true;
  ASSERT_EQ(napi_ok, napi_is_arraybuffer(env_, obj, &isAB));
  EXPECT_FALSE(isAB);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsArrayBuffer_NumberIsFalse) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 3.14, &num));

  bool isAB = true;
  ASSERT_EQ(napi_ok, napi_is_arraybuffer(env_, num, &isAB));
  EXPECT_FALSE(isAB);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsArrayBuffer_ArrayIsFalse) {
  napi_handle_scope scope = openScope(env_);

  napi_value arr = nullptr;
  ASSERT_EQ(napi_ok, napi_create_array(env_, &arr));

  bool isAB = true;
  ASSERT_EQ(napi_ok, napi_is_arraybuffer(env_, arr, &isAB));
  EXPECT_FALSE(isAB);

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_arraybuffer_info - argument validation
//===========================================================================

TEST_F(NapiTestFixture, GetArrayBufferInfo_NullEnv) {
  EXPECT_EQ(
      napi_invalid_arg,
      napi_get_arraybuffer_info(nullptr, nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, GetArrayBufferInfo_NullValue) {
  EXPECT_EQ(
      napi_invalid_arg,
      napi_get_arraybuffer_info(env_, nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, GetArrayBufferInfo_WrongType) {
  napi_handle_scope scope = openScope(env_);

  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));
  EXPECT_EQ(
      napi_invalid_arg, napi_get_arraybuffer_info(env_, obj, nullptr, nullptr));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_arraybuffer_info - optional output params
//===========================================================================

TEST_F(NapiTestFixture, GetArrayBufferInfo_DataOnly) {
  napi_handle_scope scope = openScope(env_);

  napi_value ab = nullptr;
  ASSERT_EQ(napi_ok, napi_create_arraybuffer(env_, 8, nullptr, &ab));

  void *data = nullptr;
  ASSERT_EQ(napi_ok, napi_get_arraybuffer_info(env_, ab, &data, nullptr));
  EXPECT_NE(nullptr, data);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetArrayBufferInfo_LengthOnly) {
  napi_handle_scope scope = openScope(env_);

  napi_value ab = nullptr;
  ASSERT_EQ(napi_ok, napi_create_arraybuffer(env_, 64, nullptr, &ab));

  size_t byteLen = 0;
  ASSERT_EQ(napi_ok, napi_get_arraybuffer_info(env_, ab, nullptr, &byteLen));
  EXPECT_EQ(64u, byteLen);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetArrayBufferInfo_BothNull) {
  // Both output params null — should still succeed.
  napi_handle_scope scope = openScope(env_);

  napi_value ab = nullptr;
  ASSERT_EQ(napi_ok, napi_create_arraybuffer(env_, 8, nullptr, &ab));

  EXPECT_EQ(napi_ok, napi_get_arraybuffer_info(env_, ab, nullptr, nullptr));

  closeScope(env_, scope);
}

//===========================================================================
// node_api_create_buffer_from_arraybuffer - bounds validation
//===========================================================================

TEST_F(
    NapiTestFixture,
    CreateBufferFromArrayBuffer_OffsetPlusLengthOutOfBounds) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = nullptr;
  ASSERT_EQ(napi_ok, napi_create_arraybuffer(env_, 16, nullptr, &ab));

  napi_value buf = nullptr;
  EXPECT_EQ(
      napi_invalid_arg,
      node_api_create_buffer_from_arraybuffer(env_, ab, 10, 8, &buf));
  closeScope(env_, scope);
}

// byte_offset + byte_length is computed in size_t and then narrowed to the
// view's uint32 size_type. A byte_offset near SIZE_MAX makes the sum wrap so
// the bounds check passes, after which the truncated offset lands far outside
// the 16-byte buffer.
TEST_F(NapiTestFixture, CreateBufferFromArrayBuffer_OffsetLengthOverflow) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = nullptr;
  ASSERT_EQ(napi_ok, napi_create_arraybuffer(env_, 16, nullptr, &ab));

  napi_value buf = nullptr;
  size_t hugeOffset = static_cast<size_t>(-1) - 3; // SIZE_MAX - 3
  EXPECT_EQ(
      napi_invalid_arg,
      node_api_create_buffer_from_arraybuffer(env_, ab, hugeOffset, 8, &buf));
  closeScope(env_, scope);
}

} // namespace

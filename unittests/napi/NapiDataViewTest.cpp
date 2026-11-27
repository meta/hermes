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

/// Helper to create an ArrayBuffer of the given byte length.
static napi_value
createArrayBuffer(napi_env env, size_t byteLen, void **data = nullptr) {
  napi_value ab = nullptr;
  EXPECT_EQ(napi_ok, napi_create_arraybuffer(env, byteLen, data, &ab));
  return ab;
}

//===========================================================================
// napi_is_dataview - argument validation
//===========================================================================

TEST_F(NapiTestFixture, IsDataView_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_is_dataview(nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, IsDataView_NullValue) {
  bool result = false;
  EXPECT_EQ(napi_invalid_arg, napi_is_dataview(env_, nullptr, &result));
}

TEST_F(NapiTestFixture, IsDataView_NullResult) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);
  napi_value dv = nullptr;
  ASSERT_EQ(napi_ok, napi_create_dataview(env_, 16, ab, 0, &dv));
  EXPECT_EQ(napi_invalid_arg, napi_is_dataview(env_, dv, nullptr));
  closeScope(env_, scope);
}

//===========================================================================
// napi_is_dataview - type discrimination
//===========================================================================

TEST_F(NapiTestFixture, IsDataView_DataViewIsTrue) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);
  napi_value dv = nullptr;
  ASSERT_EQ(napi_ok, napi_create_dataview(env_, 16, ab, 0, &dv));

  bool isDV = false;
  ASSERT_EQ(napi_ok, napi_is_dataview(env_, dv, &isDV));
  EXPECT_TRUE(isDV);
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsDataView_ObjectIsFalse) {
  napi_handle_scope scope = openScope(env_);
  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  bool isDV = true;
  ASSERT_EQ(napi_ok, napi_is_dataview(env_, obj, &isDV));
  EXPECT_FALSE(isDV);
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsDataView_ArrayBufferIsFalse) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);

  bool isDV = true;
  ASSERT_EQ(napi_ok, napi_is_dataview(env_, ab, &isDV));
  EXPECT_FALSE(isDV);
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsDataView_NumberIsFalse) {
  napi_handle_scope scope = openScope(env_);
  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 42.0, &num));

  bool isDV = true;
  ASSERT_EQ(napi_ok, napi_is_dataview(env_, num, &isDV));
  EXPECT_FALSE(isDV);
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsDataView_TypedArrayIsFalse) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);
  napi_value ta = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_typedarray(env_, napi_uint8_array, 16, ab, 0, &ta));

  bool isDV = true;
  ASSERT_EQ(napi_ok, napi_is_dataview(env_, ta, &isDV));
  EXPECT_FALSE(isDV);
  closeScope(env_, scope);
}

//===========================================================================
// napi_create_dataview - argument validation
//===========================================================================

TEST_F(NapiTestFixture, CreateDataView_NullEnv) {
  EXPECT_EQ(
      napi_invalid_arg, napi_create_dataview(nullptr, 0, nullptr, 0, nullptr));
}

TEST_F(NapiTestFixture, CreateDataView_NullArrayBuffer) {
  napi_value result = nullptr;
  EXPECT_EQ(
      napi_invalid_arg, napi_create_dataview(env_, 0, nullptr, 0, &result));
}

TEST_F(NapiTestFixture, CreateDataView_NullResult) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);
  EXPECT_EQ(napi_invalid_arg, napi_create_dataview(env_, 16, ab, 0, nullptr));
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDataView_NotArrayBuffer) {
  napi_handle_scope scope = openScope(env_);
  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value dv = nullptr;
  EXPECT_EQ(napi_invalid_arg, napi_create_dataview(env_, 0, obj, 0, &dv));
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDataView_OutOfBounds) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);

  napi_value dv = nullptr;
  // length exceeds buffer size
  EXPECT_EQ(napi_invalid_arg, napi_create_dataview(env_, 17, ab, 0, &dv));
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDataView_OffsetPlusLengthOutOfBounds) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);

  napi_value dv = nullptr;
  // offset + length > buffer size
  EXPECT_EQ(napi_invalid_arg, napi_create_dataview(env_, 8, ab, 10, &dv));
  closeScope(env_, scope);
}

// byte_offset + length is computed in size_t and then narrowed to the view's
// uint32 size_type. A byte_offset near SIZE_MAX makes the sum wrap so the
// bounds check passes, after which the truncated offset lands far outside the
// 16-byte buffer.
TEST_F(NapiTestFixture, CreateDataView_OffsetLengthOverflow) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);

  napi_value dv = nullptr;
  size_t hugeOffset = static_cast<size_t>(-1) - 3; // SIZE_MAX - 3
  EXPECT_EQ(
      napi_invalid_arg, napi_create_dataview(env_, 8, ab, hugeOffset, &dv));
  closeScope(env_, scope);
}

//===========================================================================
// napi_create_dataview - success cases
//===========================================================================

TEST_F(NapiTestFixture, CreateDataView_WholeBuffer) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 32);

  napi_value dv = nullptr;
  ASSERT_EQ(napi_ok, napi_create_dataview(env_, 32, ab, 0, &dv));
  ASSERT_NE(dv, nullptr);

  bool isDV = false;
  ASSERT_EQ(napi_ok, napi_is_dataview(env_, dv, &isDV));
  EXPECT_TRUE(isDV);
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDataView_WithOffset) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 32);

  napi_value dv = nullptr;
  ASSERT_EQ(napi_ok, napi_create_dataview(env_, 16, ab, 8, &dv));
  ASSERT_NE(dv, nullptr);

  bool isDV = false;
  ASSERT_EQ(napi_ok, napi_is_dataview(env_, dv, &isDV));
  EXPECT_TRUE(isDV);
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDataView_ZeroLength) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);

  napi_value dv = nullptr;
  ASSERT_EQ(napi_ok, napi_create_dataview(env_, 0, ab, 0, &dv));
  ASSERT_NE(dv, nullptr);

  bool isDV = false;
  ASSERT_EQ(napi_ok, napi_is_dataview(env_, dv, &isDV));
  EXPECT_TRUE(isDV);
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDataView_ZeroLengthAtEnd) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);

  napi_value dv = nullptr;
  ASSERT_EQ(napi_ok, napi_create_dataview(env_, 0, ab, 16, &dv));
  ASSERT_NE(dv, nullptr);

  bool isDV = false;
  ASSERT_EQ(napi_ok, napi_is_dataview(env_, dv, &isDV));
  EXPECT_TRUE(isDV);
  closeScope(env_, scope);
}

//===========================================================================
// napi_get_dataview_info - argument validation
//===========================================================================

TEST_F(NapiTestFixture, GetDataViewInfo_NullEnv) {
  EXPECT_EQ(
      napi_invalid_arg,
      napi_get_dataview_info(
          nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, GetDataViewInfo_NullDataView) {
  size_t len = 0;
  EXPECT_EQ(
      napi_invalid_arg,
      napi_get_dataview_info(env_, nullptr, &len, nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, GetDataViewInfo_NotDataView) {
  napi_handle_scope scope = openScope(env_);
  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  size_t len = 0;
  EXPECT_EQ(
      napi_invalid_arg,
      napi_get_dataview_info(env_, obj, &len, nullptr, nullptr, nullptr));
  closeScope(env_, scope);
}

//===========================================================================
// napi_get_dataview_info - success cases
//===========================================================================

TEST_F(NapiTestFixture, GetDataViewInfo_ByteLength) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 32);
  napi_value dv = nullptr;
  ASSERT_EQ(napi_ok, napi_create_dataview(env_, 16, ab, 8, &dv));

  size_t byteLength = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_dataview_info(env_, dv, &byteLength, nullptr, nullptr, nullptr));
  EXPECT_EQ(16u, byteLength);
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetDataViewInfo_ByteOffset) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 32);
  napi_value dv = nullptr;
  ASSERT_EQ(napi_ok, napi_create_dataview(env_, 16, ab, 8, &dv));

  size_t byteOffset = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_dataview_info(env_, dv, nullptr, nullptr, nullptr, &byteOffset));
  EXPECT_EQ(8u, byteOffset);
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetDataViewInfo_DataPointer) {
  napi_handle_scope scope = openScope(env_);
  void *abData = nullptr;
  napi_value ab = createArrayBuffer(env_, 32, &abData);
  napi_value dv = nullptr;
  ASSERT_EQ(napi_ok, napi_create_dataview(env_, 16, ab, 8, &dv));

  void *dvData = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_get_dataview_info(env_, dv, nullptr, &dvData, nullptr, nullptr));
  // DataView data should point to abData + byte_offset.
  EXPECT_EQ(static_cast<uint8_t *>(abData) + 8, dvData);
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetDataViewInfo_ArrayBuffer) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 32);
  napi_value dv = nullptr;
  ASSERT_EQ(napi_ok, napi_create_dataview(env_, 16, ab, 8, &dv));

  napi_value dvAb = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_get_dataview_info(env_, dv, nullptr, nullptr, &dvAb, nullptr));
  ASSERT_NE(dvAb, nullptr);

  // The returned arraybuffer should be an ArrayBuffer.
  bool isAB = false;
  ASSERT_EQ(napi_ok, napi_is_arraybuffer(env_, dvAb, &isAB));
  EXPECT_TRUE(isAB);

  // Verify it's the same buffer by checking size and data pointer.
  void *abData = nullptr;
  size_t abLen = 0;
  ASSERT_EQ(napi_ok, napi_get_arraybuffer_info(env_, ab, &abData, &abLen));
  void *dvAbData = nullptr;
  size_t dvAbLen = 0;
  ASSERT_EQ(
      napi_ok, napi_get_arraybuffer_info(env_, dvAb, &dvAbData, &dvAbLen));
  EXPECT_EQ(abData, dvAbData);
  EXPECT_EQ(abLen, dvAbLen);
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetDataViewInfo_AllOutputs) {
  napi_handle_scope scope = openScope(env_);
  void *abData = nullptr;
  napi_value ab = createArrayBuffer(env_, 64, &abData);
  napi_value dv = nullptr;
  ASSERT_EQ(napi_ok, napi_create_dataview(env_, 20, ab, 12, &dv));

  size_t byteLength = 0;
  void *data = nullptr;
  napi_value dvAb = nullptr;
  size_t byteOffset = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_dataview_info(env_, dv, &byteLength, &data, &dvAb, &byteOffset));

  EXPECT_EQ(20u, byteLength);
  EXPECT_EQ(static_cast<uint8_t *>(abData) + 12, data);
  EXPECT_EQ(12u, byteOffset);

  bool isAB = false;
  ASSERT_EQ(napi_ok, napi_is_arraybuffer(env_, dvAb, &isAB));
  EXPECT_TRUE(isAB);
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetDataViewInfo_AllNull) {
  // All output params are optional — passing all null should work.
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);
  napi_value dv = nullptr;
  ASSERT_EQ(napi_ok, napi_create_dataview(env_, 16, ab, 0, &dv));

  EXPECT_EQ(
      napi_ok,
      napi_get_dataview_info(env_, dv, nullptr, nullptr, nullptr, nullptr));
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetDataViewInfo_ZeroLengthView) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);
  napi_value dv = nullptr;
  ASSERT_EQ(napi_ok, napi_create_dataview(env_, 0, ab, 8, &dv));

  size_t byteLength = 99;
  size_t byteOffset = 99;
  ASSERT_EQ(
      napi_ok,
      napi_get_dataview_info(
          env_, dv, &byteLength, nullptr, nullptr, &byteOffset));
  EXPECT_EQ(0u, byteLength);
  EXPECT_EQ(8u, byteOffset);
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateDataView_DataReadWrite) {
  // Verify that data written through the ArrayBuffer pointer is
  // visible through the DataView data pointer.
  napi_handle_scope scope = openScope(env_);
  void *abData = nullptr;
  napi_value ab = createArrayBuffer(env_, 32, &abData);
  napi_value dv = nullptr;
  ASSERT_EQ(napi_ok, napi_create_dataview(env_, 8, ab, 4, &dv));

  // Write to the ArrayBuffer at offset 4.
  uint8_t *abBytes = static_cast<uint8_t *>(abData);
  abBytes[4] = 0xAA;
  abBytes[5] = 0xBB;
  abBytes[6] = 0xCC;
  abBytes[7] = 0xDD;

  // Read through DataView info.
  void *dvData = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_get_dataview_info(env_, dv, nullptr, &dvData, nullptr, nullptr));
  uint8_t *dvBytes = static_cast<uint8_t *>(dvData);
  EXPECT_EQ(0xAA, dvBytes[0]);
  EXPECT_EQ(0xBB, dvBytes[1]);
  EXPECT_EQ(0xCC, dvBytes[2]);
  EXPECT_EQ(0xDD, dvBytes[3]);
  closeScope(env_, scope);
}

} // namespace

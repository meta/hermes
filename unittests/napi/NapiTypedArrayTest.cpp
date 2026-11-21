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
static napi_value createArrayBuffer(napi_env env, size_t byteLen) {
  napi_value ab = nullptr;
  EXPECT_EQ(napi_ok, napi_create_arraybuffer(env, byteLen, nullptr, &ab));
  return ab;
}

//===========================================================================
// napi_is_typedarray - argument validation
//===========================================================================

TEST_F(NapiTestFixture, IsTypedArray_NullEnv) {
  EXPECT_EQ(napi_invalid_arg, napi_is_typedarray(nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, IsTypedArray_NullValue) {
  bool result = false;
  EXPECT_EQ(napi_invalid_arg, napi_is_typedarray(env_, nullptr, &result));
}

TEST_F(NapiTestFixture, IsTypedArray_NullResult) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);
  napi_value ta = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_typedarray(env_, napi_uint8_array, 16, ab, 0, &ta));
  EXPECT_EQ(napi_invalid_arg, napi_is_typedarray(env_, ta, nullptr));
  closeScope(env_, scope);
}

//===========================================================================
// napi_is_typedarray - type discrimination
//===========================================================================

TEST_F(NapiTestFixture, IsTypedArray_TypedArrayIsTrue) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);
  napi_value ta = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_typedarray(env_, napi_uint8_array, 16, ab, 0, &ta));

  bool isTA = false;
  ASSERT_EQ(napi_ok, napi_is_typedarray(env_, ta, &isTA));
  EXPECT_TRUE(isTA);
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsTypedArray_ArrayBufferIsFalse) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);

  bool isTA = true;
  ASSERT_EQ(napi_ok, napi_is_typedarray(env_, ab, &isTA));
  EXPECT_FALSE(isTA);
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsTypedArray_ObjectIsFalse) {
  napi_handle_scope scope = openScope(env_);
  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  bool isTA = true;
  ASSERT_EQ(napi_ok, napi_is_typedarray(env_, obj, &isTA));
  EXPECT_FALSE(isTA);
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, IsTypedArray_NumberIsFalse) {
  napi_handle_scope scope = openScope(env_);
  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 42.0, &num));

  bool isTA = true;
  ASSERT_EQ(napi_ok, napi_is_typedarray(env_, num, &isTA));
  EXPECT_FALSE(isTA);
  closeScope(env_, scope);
}

//===========================================================================
// napi_create_typedarray - argument validation
//===========================================================================

TEST_F(NapiTestFixture, CreateTypedArray_NullEnv) {
  EXPECT_EQ(
      napi_invalid_arg,
      napi_create_typedarray(
          nullptr, napi_uint8_array, 0, nullptr, 0, nullptr));
}

TEST_F(NapiTestFixture, CreateTypedArray_NullArrayBuffer) {
  napi_handle_scope scope = openScope(env_);
  napi_value result = nullptr;
  EXPECT_EQ(
      napi_invalid_arg,
      napi_create_typedarray(env_, napi_uint8_array, 0, nullptr, 0, &result));
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateTypedArray_NullResult) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);
  EXPECT_EQ(
      napi_invalid_arg,
      napi_create_typedarray(env_, napi_uint8_array, 16, ab, 0, nullptr));
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateTypedArray_NotArrayBuffer) {
  napi_handle_scope scope = openScope(env_);
  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));

  napi_value result = nullptr;
  EXPECT_EQ(
      napi_invalid_arg,
      napi_create_typedarray(env_, napi_uint8_array, 0, obj, 0, &result));
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateTypedArray_MisalignedOffset) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);

  // Int32Array requires 4-byte alignment. Offset 1 is misaligned.
  napi_value result = nullptr;
  EXPECT_EQ(
      napi_invalid_arg,
      napi_create_typedarray(env_, napi_int32_array, 1, ab, 1, &result));
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateTypedArray_OutOfBounds) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);

  // Requesting 5 Int32 elements = 20 bytes > 16 bytes.
  napi_value result = nullptr;
  EXPECT_EQ(
      napi_invalid_arg,
      napi_create_typedarray(env_, napi_int32_array, 5, ab, 0, &result));
  closeScope(env_, scope);
}

// byte_offset + length * elemSize is computed in size_t and then narrowed to
// the view's uint32 size_type. A byte_offset near SIZE_MAX makes the sum wrap
// so the bounds check passes, after which the truncated offset lands far
// outside the 16-byte buffer.
TEST_F(NapiTestFixture, CreateTypedArray_OffsetLengthOverflow) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);

  napi_value result = nullptr;
  size_t hugeOffset = static_cast<size_t>(-1) - 3; // SIZE_MAX - 3
  EXPECT_EQ(
      napi_invalid_arg,
      napi_create_typedarray(
          env_, napi_uint8_array, 8, ab, hugeOffset, &result));
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateTypedArray_OffsetPlusLengthOutOfBounds) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);

  // offset=8 + 3*4=12 = 20 > 16
  napi_value result = nullptr;
  EXPECT_EQ(
      napi_invalid_arg,
      napi_create_typedarray(env_, napi_int32_array, 3, ab, 8, &result));
  closeScope(env_, scope);
}

//===========================================================================
// napi_create_typedarray - basic creation for each type
//===========================================================================

/// Test creating each TypedArray type with the correct element size.
struct TypedArrayTypeInfo {
  napi_typedarray_type type;
  uint8_t elemSize;
  const char *name;
};

static const TypedArrayTypeInfo kTypedArrayTypes[] = {
    {napi_int8_array, 1, "Int8Array"},
    {napi_uint8_array, 1, "Uint8Array"},
    {napi_uint8_clamped_array, 1, "Uint8ClampedArray"},
    {napi_int16_array, 2, "Int16Array"},
    {napi_uint16_array, 2, "Uint16Array"},
    {napi_int32_array, 4, "Int32Array"},
    {napi_uint32_array, 4, "Uint32Array"},
    {napi_float32_array, 4, "Float32Array"},
    {napi_float64_array, 8, "Float64Array"},
    {napi_bigint64_array, 8, "BigInt64Array"},
    {napi_biguint64_array, 8, "BigUint64Array"},
};

TEST_F(NapiTestFixture, CreateTypedArray_AllTypes) {
  napi_handle_scope scope = openScope(env_);

  // Create a buffer large enough for all types (64 bytes).
  napi_value ab = createArrayBuffer(env_, 64);

  for (const auto &info : kTypedArrayTypes) {
    size_t numElems = 8 / info.elemSize;
    napi_value ta = nullptr;
    ASSERT_EQ(
        napi_ok, napi_create_typedarray(env_, info.type, numElems, ab, 0, &ta))
        << "Failed to create " << info.name;

    // Verify it is a TypedArray.
    bool isTA = false;
    ASSERT_EQ(napi_ok, napi_is_typedarray(env_, ta, &isTA));
    EXPECT_TRUE(isTA) << info.name << " should be a TypedArray";

    // Verify typeof is object.
    napi_valuetype vtype = napi_undefined;
    ASSERT_EQ(napi_ok, napi_typeof(env_, ta, &vtype));
    EXPECT_EQ(napi_object, vtype) << info.name;
  }

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_typedarray_info - basic info retrieval
//===========================================================================

TEST_F(NapiTestFixture, GetTypedArrayInfo_NullEnv) {
  EXPECT_EQ(
      napi_invalid_arg,
      napi_get_typedarray_info(
          nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, GetTypedArrayInfo_NullValue) {
  EXPECT_EQ(
      napi_invalid_arg,
      napi_get_typedarray_info(
          env_, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, GetTypedArrayInfo_WrongType) {
  napi_handle_scope scope = openScope(env_);
  napi_value obj = nullptr;
  ASSERT_EQ(napi_ok, napi_create_object(env_, &obj));
  EXPECT_EQ(
      napi_invalid_arg,
      napi_get_typedarray_info(
          env_, obj, nullptr, nullptr, nullptr, nullptr, nullptr));
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetTypedArrayInfo_AllOutputParams) {
  napi_handle_scope scope = openScope(env_);

  // Create an ArrayBuffer with 32 bytes.
  void *abData = nullptr;
  napi_value ab = nullptr;
  ASSERT_EQ(napi_ok, napi_create_arraybuffer(env_, 32, &abData, &ab));

  // Create an Int32Array of length 4 at offset 8.
  napi_value ta = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_typedarray(env_, napi_int32_array, 4, ab, 8, &ta));

  // Get info.
  napi_typedarray_type type;
  size_t length = 0;
  void *data = nullptr;
  napi_value outAb = nullptr;
  size_t byteOffset = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_typedarray_info(
          env_, ta, &type, &length, &data, &outAb, &byteOffset));

  EXPECT_EQ(napi_int32_array, type);
  EXPECT_EQ(4u, length);
  EXPECT_EQ(8u, byteOffset);
  // data should point to abData + 8.
  EXPECT_EQ(static_cast<uint8_t *>(abData) + 8, static_cast<uint8_t *>(data));

  // outAb should be an ArrayBuffer.
  bool isAB = false;
  ASSERT_EQ(napi_ok, napi_is_arraybuffer(env_, outAb, &isAB));
  EXPECT_TRUE(isAB);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetTypedArrayInfo_TypeForEachKind) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 64);

  for (const auto &info : kTypedArrayTypes) {
    size_t numElems = 8 / info.elemSize;
    napi_value ta = nullptr;
    ASSERT_EQ(
        napi_ok, napi_create_typedarray(env_, info.type, numElems, ab, 0, &ta))
        << info.name;

    napi_typedarray_type gotType;
    size_t gotLength = 0;
    ASSERT_EQ(
        napi_ok,
        napi_get_typedarray_info(
            env_, ta, &gotType, &gotLength, nullptr, nullptr, nullptr))
        << info.name;

    EXPECT_EQ(info.type, gotType) << info.name;
    EXPECT_EQ(numElems, gotLength) << info.name;
  }

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetTypedArrayInfo_AllOutputsNull) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 16);
  napi_value ta = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_typedarray(env_, napi_uint8_array, 16, ab, 0, &ta));

  // All output pointers null — should still succeed.
  EXPECT_EQ(
      napi_ok,
      napi_get_typedarray_info(
          env_, ta, nullptr, nullptr, nullptr, nullptr, nullptr));

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_typedarray - write and read data
//===========================================================================

TEST_F(NapiTestFixture, CreateTypedArray_WriteReadUint8) {
  napi_handle_scope scope = openScope(env_);

  void *abData = nullptr;
  napi_value ab = nullptr;
  ASSERT_EQ(napi_ok, napi_create_arraybuffer(env_, 8, &abData, &ab));

  napi_value ta = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_typedarray(env_, napi_uint8_array, 8, ab, 0, &ta));

  // Write data via the TypedArray's data pointer.
  void *taData = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_get_typedarray_info(
          env_, ta, nullptr, nullptr, &taData, nullptr, nullptr));

  auto *bytes = static_cast<uint8_t *>(taData);
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<uint8_t>(i + 10);
  }

  // Read back via the ArrayBuffer data pointer.
  auto *abBytes = static_cast<uint8_t *>(abData);
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(static_cast<uint8_t>(i + 10), abBytes[i]);
  }

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateTypedArray_WriteReadInt32) {
  napi_handle_scope scope = openScope(env_);

  void *abData = nullptr;
  napi_value ab = nullptr;
  ASSERT_EQ(napi_ok, napi_create_arraybuffer(env_, 16, &abData, &ab));

  napi_value ta = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_typedarray(env_, napi_int32_array, 4, ab, 0, &ta));

  void *taData = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_get_typedarray_info(
          env_, ta, nullptr, nullptr, &taData, nullptr, nullptr));

  auto *ints = static_cast<int32_t *>(taData);
  ints[0] = -100;
  ints[1] = 0;
  ints[2] = 42;
  ints[3] = 2147483647;

  // Verify via ArrayBuffer.
  auto *abInts = static_cast<int32_t *>(abData);
  EXPECT_EQ(-100, abInts[0]);
  EXPECT_EQ(0, abInts[1]);
  EXPECT_EQ(42, abInts[2]);
  EXPECT_EQ(2147483647, abInts[3]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateTypedArray_WriteReadFloat64) {
  napi_handle_scope scope = openScope(env_);

  void *abData = nullptr;
  napi_value ab = nullptr;
  ASSERT_EQ(napi_ok, napi_create_arraybuffer(env_, 16, &abData, &ab));

  napi_value ta = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_typedarray(env_, napi_float64_array, 2, ab, 0, &ta));

  void *taData = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_get_typedarray_info(
          env_, ta, nullptr, nullptr, &taData, nullptr, nullptr));

  auto *doubles = static_cast<double *>(taData);
  doubles[0] = 3.14;
  doubles[1] = -1.0e10;

  auto *abDoubles = static_cast<double *>(abData);
  EXPECT_DOUBLE_EQ(3.14, abDoubles[0]);
  EXPECT_DOUBLE_EQ(-1.0e10, abDoubles[1]);

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_typedarray - with byte offset
//===========================================================================

TEST_F(NapiTestFixture, CreateTypedArray_WithByteOffset) {
  napi_handle_scope scope = openScope(env_);

  void *abData = nullptr;
  napi_value ab = nullptr;
  ASSERT_EQ(napi_ok, napi_create_arraybuffer(env_, 32, &abData, &ab));

  // Create an Int32Array at byte offset 12 with 2 elements (8 bytes).
  napi_value ta = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_typedarray(env_, napi_int32_array, 2, ab, 12, &ta));

  napi_typedarray_type type;
  size_t length = 0;
  void *data = nullptr;
  size_t byteOffset = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_typedarray_info(
          env_, ta, &type, &length, &data, nullptr, &byteOffset));

  EXPECT_EQ(napi_int32_array, type);
  EXPECT_EQ(2u, length);
  EXPECT_EQ(12u, byteOffset);
  EXPECT_EQ(static_cast<uint8_t *>(abData) + 12, static_cast<uint8_t *>(data));

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_typedarray - zero length
//===========================================================================

TEST_F(NapiTestFixture, CreateTypedArray_ZeroLength) {
  napi_handle_scope scope = openScope(env_);
  napi_value ab = createArrayBuffer(env_, 0);

  napi_value ta = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_typedarray(env_, napi_uint8_array, 0, ab, 0, &ta));

  bool isTA = false;
  ASSERT_EQ(napi_ok, napi_is_typedarray(env_, ta, &isTA));
  EXPECT_TRUE(isTA);

  size_t length = 99;
  ASSERT_EQ(
      napi_ok,
      napi_get_typedarray_info(
          env_, ta, nullptr, &length, nullptr, nullptr, nullptr));
  EXPECT_EQ(0u, length);

  closeScope(env_, scope);
}

//===========================================================================
// Multiple TypedArrays sharing the same ArrayBuffer
//===========================================================================

TEST_F(NapiTestFixture, CreateTypedArray_SharedArrayBuffer) {
  napi_handle_scope scope = openScope(env_);

  void *abData = nullptr;
  napi_value ab = nullptr;
  ASSERT_EQ(napi_ok, napi_create_arraybuffer(env_, 16, &abData, &ab));

  // Create two Uint8Arrays over different regions of the same buffer.
  napi_value ta1 = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_typedarray(env_, napi_uint8_array, 8, ab, 0, &ta1));

  napi_value ta2 = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_typedarray(env_, napi_uint8_array, 8, ab, 8, &ta2));

  // Write to ta1, verify via ta2 and abData.
  void *data1 = nullptr;
  void *data2 = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_get_typedarray_info(
          env_, ta1, nullptr, nullptr, &data1, nullptr, nullptr));
  ASSERT_EQ(
      napi_ok,
      napi_get_typedarray_info(
          env_, ta2, nullptr, nullptr, &data2, nullptr, nullptr));

  static_cast<uint8_t *>(data1)[0] = 0xAA;
  static_cast<uint8_t *>(data2)[0] = 0xBB;

  // ta1 data at offset 0, ta2 data at offset 8.
  EXPECT_EQ(0xAA, static_cast<uint8_t *>(abData)[0]);
  EXPECT_EQ(0xBB, static_cast<uint8_t *>(abData)[8]);

  closeScope(env_, scope);
}

} // namespace

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Support/sh_tryfast_fp_cvt.h"

#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

namespace {

TEST(test_sh_fp_trunc, TryFastF64ToI32) {
  int32_t result;

  // In range
  double x = 123.0;
  EXPECT_TRUE(sh_tryfast_f64_to_i32(x, result));
  EXPECT_EQ(result, 123);

  // Out of range
  x = 1e10;
  EXPECT_FALSE(sh_tryfast_f64_to_i32(x, result));

  x = -123.0;
  EXPECT_TRUE(sh_tryfast_f64_to_i32(x, result));
  EXPECT_EQ(result, -123);

  x = 0.0;
  EXPECT_TRUE(sh_tryfast_f64_to_i32(x, result));
  EXPECT_EQ(result, 0);

  x = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(sh_tryfast_f64_to_i32(x, result));

  x = -std::numeric_limits<double>::infinity();
  EXPECT_FALSE(sh_tryfast_f64_to_i32(x, result));
}

TEST(test_sh_fp_trunc, TryFastF64ToU32) {
  uint32_t result;

  // In range
  double x = 123.0;
  EXPECT_TRUE(sh_tryfast_f64_to_u32(x, result));
  EXPECT_EQ(result, 123);

  // Out of range (negative value)
  x = -1.0;
  EXPECT_FALSE(sh_tryfast_f64_to_u32(x, result));

  // Large positive value that fits into uint32_t but not int32_t
  x = 0xFFFFFFFF;
  EXPECT_TRUE(sh_tryfast_f64_to_u32(x, result));
  EXPECT_EQ(result, std::numeric_limits<uint32_t>::max());

  // Large positive value that doesn't fit into uint32_t
  x = 1e10;
  EXPECT_FALSE(sh_tryfast_f64_to_u32(x, result));

  x = 0.0;
  EXPECT_TRUE(sh_tryfast_f64_to_u32(x, result));
  EXPECT_EQ(result, 0);

  x = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(sh_tryfast_f64_to_u32(x, result));

  x = -std::numeric_limits<double>::infinity();
  EXPECT_FALSE(sh_tryfast_f64_to_u32(x, result));
}

TEST(test_sh_fp_trunc, TryFastF64ToI64) {
  int64_t result;

  // In range
  double x = 1234567890123.0;
  EXPECT_TRUE(sh_tryfast_f64_to_i64(x, result));
  EXPECT_EQ(result, 1234567890123);

  // Out of range
  x = 1e20; // A value that is out of range for int64_t
  EXPECT_FALSE(sh_tryfast_f64_to_i64(x, result));

  // Negative values
  x = -4.0;
  EXPECT_TRUE(sh_tryfast_f64_to_i64(x, result));
  EXPECT_EQ(result, -4);
  x = -255.0;
  EXPECT_TRUE(sh_tryfast_f64_to_i64(x, result));
  EXPECT_EQ(result, -255);
  x = -4611686018427387904.0;
  EXPECT_TRUE(sh_tryfast_f64_to_i64(x, result));
  EXPECT_EQ(result, -4611686018427387904);

  x = 0.0;
  EXPECT_TRUE(sh_tryfast_f64_to_i64(x, result));
  EXPECT_EQ(result, 0);

  x = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(sh_tryfast_f64_to_i64(x, result));

  x = -std::numeric_limits<double>::infinity();
  EXPECT_FALSE(sh_tryfast_f64_to_i64(x, result));
}

TEST(test_sh_fp_trunc, TryFastF64ToU64) {
  uint64_t result;

  // In range
  double x = 1234567890123.0;
  EXPECT_TRUE(sh_tryfast_f64_to_u64(x, result));
  EXPECT_EQ(result, 1234567890123);

  // Out of range (negative value)
  x = -1.0;
  EXPECT_FALSE(sh_tryfast_f64_to_u64(x, result));

  // Large positive value.
  x = 0x7FFF000000000000ull;
  EXPECT_TRUE(sh_tryfast_f64_to_u64(x, result));
  EXPECT_EQ(result, 0x7FFF000000000000ull);

  // Large positive value that doesn't fit into uint64_t
  x = 1e20;
  EXPECT_FALSE(sh_tryfast_f64_to_u64(x, result));

  x = 0.0;
  EXPECT_TRUE(sh_tryfast_f64_to_u64(x, result));
  EXPECT_EQ(result, 0);

  x = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(sh_tryfast_f64_to_u64(x, result));

  x = -std::numeric_limits<double>::infinity();
  EXPECT_FALSE(sh_tryfast_f64_to_u64(x, result));
}

TEST(test_sh_fp_trunc, DynamicNonIntegers) {
  const double inputs[] = {
      -1.5,
      -0.5,
      0.5,
      1.5,
      std::numeric_limits<double>::denorm_min(),
      -std::numeric_limits<double>::denorm_min(),
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN(),
  };
  for (double value : inputs) {
    // Keep the input dynamic so optimization cannot fold away the conversion.
    volatile double input = value;
    int32_t i32;
    uint32_t u32;
    int64_t i64;
    uint64_t u64;
    EXPECT_FALSE(sh_tryfast_f64_to_i32(input, i32));
    EXPECT_FALSE(sh_tryfast_f64_to_u32(input, u32));
    EXPECT_FALSE(sh_tryfast_f64_to_i64(input, i64));
    EXPECT_FALSE(sh_tryfast_f64_to_u64(input, u64));
  }
}

TEST(test_sh_fp_trunc, DynamicOverflow) {
  // The 64-bit upper bounds are also the rounded double representations of
  // INT64_MAX and UINT64_MAX. A saturated result must not round-trip to them.
  const double i32Inputs[] = {-2147483649.0, 2147483648.0, -1e20, 1e20};
  const double u32Inputs[] = {-1.0, 4294967296.0, -1e20, 1e20};
  const double i64Inputs[] = {
      -9223372036854777856.0, 9223372036854775808.0, -1e20, 1e20};
  const double u64Inputs[] = {-1.0, 18446744073709551616.0, -1e20, 1e20};
  for (double value : i32Inputs) {
    volatile double input = value;
    int32_t result;
    EXPECT_FALSE(sh_tryfast_f64_to_i32(input, result));
  }
  for (double value : u32Inputs) {
    volatile double input = value;
    uint32_t result;
    EXPECT_FALSE(sh_tryfast_f64_to_u32(input, result));
  }
  for (double value : i64Inputs) {
    volatile double input = value;
    int64_t result;
    EXPECT_FALSE(sh_tryfast_f64_to_i64(input, result));
  }
  for (double value : u64Inputs) {
    volatile double input = value;
    uint64_t result;
    EXPECT_FALSE(sh_tryfast_f64_to_u64(input, result));
  }
}

TEST(test_sh_fp_trunc, DynamicTruncate32BitFractions) {
  // The helpers promise correct truncation when the integer part is in range,
  // even when the fractional input lies outside the integer endpoints.
  volatile double belowI32Min = -2147483648.5;
  EXPECT_EQ(INT32_MIN, _sh_tryfast_f64_to_i32_cvt(belowI32Min));
  volatile double aboveI32Max = 2147483647.5;
  EXPECT_EQ(INT32_MAX, _sh_tryfast_f64_to_i32_cvt(aboveI32Max));
  volatile double belowU32Min = -0.5;
  EXPECT_EQ(0u, _sh_tryfast_f64_to_u32_cvt(belowU32Min));
  volatile double aboveU32Max = 4294967295.5;
  EXPECT_EQ(UINT32_MAX, _sh_tryfast_f64_to_u32_cvt(aboveU32Max));
}
} // namespace

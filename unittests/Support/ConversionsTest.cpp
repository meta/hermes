/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Support/Conversions.h"

#include <limits>

#include "gtest/gtest.h"

using namespace hermes;

namespace {

TEST(ConversionsTest, DynamicTruncateBoundaries) {
  const struct {
    double input;
    uint32_t expected;
  } cases[] = {
      {-1, 0xffffffffu},
      {-2147483648.0, 0x80000000u},
      {-4294967297.0, 0xffffffffu},
      {-1.5, 0xffffffffu},
      {4294967295.0, 0xffffffffu},
      {4294967296.0, 0},
      {9007199254740991.0, 0xffffffffu},
      {9223372036854775808.0, 0},
      {-9223372036854775808.0, 0},
      {18446744073709551616.0, 0},
      {1e20, 1661992960u},
      {-1e20, 2632974336u},
      {std::numeric_limits<double>::infinity(), 0},
      {-std::numeric_limits<double>::infinity(), 0},
      {std::numeric_limits<double>::quiet_NaN(), 0},
  };
  for (const auto &test : cases) {
    // Bypass the constant-folding shortcut to exercise the dynamic conversion
    // and its round-trip rejection of out-of-range floating-point values.
    volatile double input = test.input;
    EXPECT_EQ(test.expected, hermes::truncateToUInt32(input));
    EXPECT_EQ(
        static_cast<int32_t>(test.expected), hermes::truncateToInt32(input));
  }
}

TEST(ConversionsTest, toInt32Test) {
  EXPECT_EQ(0, hermes::truncateToInt32(0));
  EXPECT_EQ(0, hermes::truncateToInt32(-0.1));
  EXPECT_EQ(0, hermes::truncateToInt32(0.1));
  EXPECT_EQ(1, hermes::truncateToInt32(1));
  EXPECT_EQ(-1, hermes::truncateToInt32(-1.5));

  EXPECT_EQ(1661992960, hermes::truncateToInt32(1e20));
  EXPECT_EQ(-1661992960, hermes::truncateToInt32(-1e20));
  EXPECT_EQ(-2147483648, hermes::truncateToInt32(-2147483648));
  EXPECT_EQ(0, hermes::truncateToInt32(9223372036854775808.0));
  // This should still go through the fast/constant path even if it
  // does not fit into int32_t, because we use a wider range check.
  // It won't break the old implementation. Add it to make sure
  // we don't make mistakes in the new fast/constant path.
  EXPECT_EQ(-2147483648, hermes::truncateToInt32(2147483648));

  EXPECT_EQ(
      0, hermes::truncateToInt32(std::numeric_limits<double>::infinity()));
  EXPECT_EQ(
      0, hermes::truncateToInt32(-std::numeric_limits<double>::infinity()));
  EXPECT_EQ(
      0, hermes::truncateToInt32(-std::numeric_limits<double>::quiet_NaN()));
  EXPECT_EQ(
      0, hermes::truncateToInt32(-std::numeric_limits<double>::denorm_min()));
}

TEST(ConversionsTest, toArrayIndexTest) {
  OptValue<uint32_t> res;

  res = toArrayIndex("123");
  ASSERT_TRUE(res.hasValue());
  ASSERT_EQ(123u, *res);

  res = toArrayIndex("0");
  ASSERT_TRUE(res.hasValue());
  ASSERT_EQ(0u, *res);

  res = toArrayIndex("00");
  ASSERT_FALSE(res.hasValue());

  res = toArrayIndex("01");
  ASSERT_FALSE(res.hasValue());

  res = toArrayIndex("4294967294");
  ASSERT_TRUE(res.hasValue());
  ASSERT_EQ(0xFFFFFFFE, *res);

  res = toArrayIndex("4294967295");
  ASSERT_FALSE(res.hasValue());

  res = toArrayIndex("4294967296");
  ASSERT_FALSE(res.hasValue());

  res = toArrayIndex("99999999999999");
  ASSERT_FALSE(res.hasValue());

  res = toArrayIndex("123a");
  ASSERT_FALSE(res.hasValue());

  res = toArrayIndex(" 123");
  ASSERT_FALSE(res.hasValue());

  res = toArrayIndex("a123");
  ASSERT_FALSE(res.hasValue());

  res = toArrayIndex("4772185884");
  ASSERT_FALSE(res.hasValue());

  // Wrapper for char16_t.
  auto toArrayIndexU = [](const char16_t *str) {
    return hermes::toArrayIndex(
        str, str + std::char_traits<char16_t>::length(str));
  };

  res = toArrayIndexU(u"123");
  ASSERT_TRUE(res.hasValue());
  ASSERT_EQ(123u, *res);

  res = toArrayIndexU(u"0");
  ASSERT_TRUE(res.hasValue());
  ASSERT_EQ(0u, *res);

  res = toArrayIndexU(u"00");
  ASSERT_FALSE(res.hasValue());

  res = toArrayIndexU(u"01");
  ASSERT_FALSE(res.hasValue());

  res = toArrayIndexU(u"4294967294");
  ASSERT_TRUE(res.hasValue());
  ASSERT_EQ(0xFFFFFFFE, *res);

  res = toArrayIndexU(u"4294967295");
  ASSERT_FALSE(res.hasValue());

  res = toArrayIndexU(u"4294967296");
  ASSERT_FALSE(res.hasValue());

  res = toArrayIndexU(u"99999999999999");
  ASSERT_FALSE(res.hasValue());

  res = toArrayIndexU(u"123a");
  ASSERT_FALSE(res.hasValue());

  res = toArrayIndexU(u" 123");
  ASSERT_FALSE(res.hasValue());

  res = toArrayIndexU(u"a123");
  ASSERT_FALSE(res.hasValue());
};

TEST(ConversionsTest, float16ToDoubleTest) {
  // +0 (0x0000).
  EXPECT_EQ(0.0, hermes::float16ToDouble(0x0000));
  EXPECT_FALSE(std::signbit(hermes::float16ToDouble(0x0000)));

  // -0 (0x8000).
  EXPECT_EQ(0.0, hermes::float16ToDouble(0x8000));
  EXPECT_TRUE(std::signbit(hermes::float16ToDouble(0x8000)));

  // +1.0 (0x3C00).
  EXPECT_EQ(1.0, hermes::float16ToDouble(0x3C00));

  // -1.0 (0xBC00).
  EXPECT_EQ(-1.0, hermes::float16ToDouble(0xBC00));

  // +Infinity (0x7C00).
  EXPECT_EQ(
      std::numeric_limits<double>::infinity(), hermes::float16ToDouble(0x7C00));

  // -Infinity (0xFC00).
  EXPECT_EQ(
      -std::numeric_limits<double>::infinity(),
      hermes::float16ToDouble(0xFC00));

  // NaN (0x7E00 = quiet NaN).
  EXPECT_TRUE(std::isnan(hermes::float16ToDouble(0x7E00)));

  // NaN with sign bit (0xFE00).
  EXPECT_TRUE(std::isnan(hermes::float16ToDouble(0xFE00)));

  // Signaling NaN (0x7C01 - exponent all 1s, mantissa non-zero low bit).
  EXPECT_TRUE(std::isnan(hermes::float16ToDouble(0x7C01)));

  // Max normal: 65504.0 (0x7BFF).
  EXPECT_EQ(65504.0, hermes::float16ToDouble(0x7BFF));

  // Smallest positive normal: 2^-14 (0x0400).
  EXPECT_EQ(std::ldexp(1.0, -14), hermes::float16ToDouble(0x0400));

  // Smallest positive subnormal: 2^-24 (0x0001).
  EXPECT_EQ(std::ldexp(1.0, -24), hermes::float16ToDouble(0x0001));

  // Largest subnormal: (1 - 2^-10) * 2^-14 (0x03FF).
  EXPECT_EQ(
      std::ldexp(1.0, -14) - std::ldexp(1.0, -24),
      hermes::float16ToDouble(0x03FF));

  // 0.5 (0x3800).
  EXPECT_EQ(0.5, hermes::float16ToDouble(0x3800));

  // 2.0 (0x4000).
  EXPECT_EQ(2.0, hermes::float16ToDouble(0x4000));

  // -2.0 (0xC000).
  EXPECT_EQ(-2.0, hermes::float16ToDouble(0xC000));

  // 0.1 rounded to float16 (0x2E66 = 0.0999755859375).
  EXPECT_DOUBLE_EQ(0.0999755859375, hermes::float16ToDouble(0x2E66));

  // Subnormal 0x0200 = 2^-15.
  EXPECT_EQ(std::ldexp(1.0, -15), hermes::float16ToDouble(0x0200));
}

TEST(ConversionsTest, doubleToFloat16Test) {
  // +0.0 -> 0x0000.
  EXPECT_EQ(0x0000, hermes::doubleToFloat16(0.0));

  // -0.0 -> 0x8000.
  EXPECT_EQ(0x8000, hermes::doubleToFloat16(-0.0));

  // 1.0 -> 0x3C00.
  EXPECT_EQ(0x3C00, hermes::doubleToFloat16(1.0));

  // -1.0 -> 0xBC00.
  EXPECT_EQ(0xBC00, hermes::doubleToFloat16(-1.0));

  // +Infinity -> 0x7C00.
  EXPECT_EQ(
      0x7C00, hermes::doubleToFloat16(std::numeric_limits<double>::infinity()));

  // -Infinity -> 0xFC00.
  EXPECT_EQ(
      0xFC00,
      hermes::doubleToFloat16(-std::numeric_limits<double>::infinity()));

  // NaN -> quiet NaN 0x7E00.
  EXPECT_EQ(
      0x7E00,
      hermes::doubleToFloat16(std::numeric_limits<double>::quiet_NaN()));

  // -NaN preserves sign.
  EXPECT_EQ(
      0xFE00,
      hermes::doubleToFloat16(-std::numeric_limits<double>::quiet_NaN()));

  // Max normal float16: 65504.0 -> 0x7BFF.
  EXPECT_EQ(0x7BFF, hermes::doubleToFloat16(65504.0));

  // Overflow to +Infinity: 65520.0 (exceeds float16 max).
  EXPECT_EQ(0x7C00, hermes::doubleToFloat16(65520.0));

  // Overflow to -Infinity.
  EXPECT_EQ(0xFC00, hermes::doubleToFloat16(-65520.0));

  // Large double overflows to infinity.
  EXPECT_EQ(0x7C00, hermes::doubleToFloat16(1e20));

  // 0.5 -> 0x3800.
  EXPECT_EQ(0x3800, hermes::doubleToFloat16(0.5));

  // 2.0 -> 0x4000.
  EXPECT_EQ(0x4000, hermes::doubleToFloat16(2.0));

  // Smallest positive normal: 2^-14 -> 0x0400.
  EXPECT_EQ(0x0400, hermes::doubleToFloat16(std::ldexp(1.0, -14)));

  // Subnormal: 2^-24 (smallest subnormal) -> 0x0001.
  EXPECT_EQ(0x0001, hermes::doubleToFloat16(std::ldexp(1.0, -24)));

  // Subnormal: 2^-15 -> 0x0200.
  EXPECT_EQ(0x0200, hermes::doubleToFloat16(std::ldexp(1.0, -15)));

  // Underflow to zero: 2^-25 is below smallest subnormal.
  // Halfway between 0 and smallest subnormal (2^-24).
  // Round-to-even: 0 is even, so stays at 0.
  EXPECT_EQ(0x0000, hermes::doubleToFloat16(std::ldexp(1.0, -25)));

  // Underflow to zero: very small positive.
  EXPECT_EQ(0x0000, hermes::doubleToFloat16(1e-20));

  // Negative underflow to -0.
  EXPECT_EQ(0x8000, hermes::doubleToFloat16(-1e-20));

  // Double subnormal (magnitude < 2^-1022) -> ±0.
  EXPECT_EQ(
      0x0000,
      hermes::doubleToFloat16(std::numeric_limits<double>::denorm_min()));
  EXPECT_EQ(
      0x8000,
      hermes::doubleToFloat16(-std::numeric_limits<double>::denorm_min()));

  // Round-to-nearest-even: 1.0 + 0.5 ULP (in float16).
  // 1.0 = 0x3C00, next float16 is 1.0009765625 = 0x3C01.
  // Halfway rounds to even (1.0 = even mantissa), so stays at 0x3C00.
  // 1.0 in float16 has mantissa=0 (even). 1.0 + half ULP = 1.00048828125.
  EXPECT_EQ(0x3C00, hermes::doubleToFloat16(1.00048828125));

  // 1.0009765625 + 0.5 ULP: halfway between 0x3C01 (odd) and 0x3C02 (even).
  // Rounds up to 0x3C02 (even mantissa).
  // 1.0009765625 + half ULP = 1.001464843750.
  EXPECT_EQ(0x3C02, hermes::doubleToFloat16(1.00146484375));

  // Just above halfway: should round up.
  // 1.0005 is above the halfway point (1.00048828125) between 0x3C00 and
  // 0x3C01, so it rounds up.
  EXPECT_EQ(0x3C01, hermes::doubleToFloat16(1.0005));

  // Roundtrip: doubleToFloat16 -> float16ToDouble -> doubleToFloat16.
  auto roundtrip = [](uint16_t bits) {
    return hermes::doubleToFloat16(hermes::float16ToDouble(bits));
  };
  EXPECT_EQ(0x0000, roundtrip(0x0000)); // +0
  EXPECT_EQ(0x8000, roundtrip(0x8000)); // -0
  EXPECT_EQ(0x3C00, roundtrip(0x3C00)); // 1.0
  EXPECT_EQ(0x7BFF, roundtrip(0x7BFF)); // max normal
  EXPECT_EQ(0x7C00, roundtrip(0x7C00)); // +inf
  EXPECT_EQ(0xFC00, roundtrip(0xFC00)); // -inf
  EXPECT_EQ(0x0001, roundtrip(0x0001)); // min subnormal
  EXPECT_EQ(0x03FF, roundtrip(0x03FF)); // max subnormal
  EXPECT_EQ(0x0400, roundtrip(0x0400)); // min normal

  // Mantissa overflow rounding: 0x3FFF is max mantissa in exponent range.
  // float16 1.9990234375 (0x3FFF) + next representable is 2.0 (0x4000).
  // Verify the boundary value is representable.
  EXPECT_EQ(0x3FFF, roundtrip(0x3FFF));
}

TEST(ConversionsTest, numberToStringTest) {
  char buf[NUMBER_TO_STRING_BUF_SIZE];

#define DoubleToStringTest(expected, value)          \
  do {                                               \
    numberToStringSlowPath(value, buf, sizeof(buf)); \
    EXPECT_STREQ(expected, buf);                     \
  } while (0)

  DoubleToStringTest("NaN", std::nan(""));

  auto infinity = std::numeric_limits<double>::infinity();
  DoubleToStringTest("Infinity", infinity);
  DoubleToStringTest("-Infinity", -infinity);

  DoubleToStringTest("0", +0.0);
  DoubleToStringTest("0", -0.0);
  DoubleToStringTest("3", 3);
  DoubleToStringTest("-3", -3);
  DoubleToStringTest("-10000", -10000);
  DoubleToStringTest("1100000", 1.1e6);

  DoubleToStringTest("10000", 10000);
  DoubleToStringTest("100000", 100000);
  DoubleToStringTest("1000000", 1000000);
  DoubleToStringTest("10000000", 10000000);
  DoubleToStringTest("100000000000000000000", 1e20);
  DoubleToStringTest("1e+21", 1e21);

  DoubleToStringTest("0.125", 0.125);
  DoubleToStringTest("3.14", 3.14);
  DoubleToStringTest("3.14", 3.14);
  DoubleToStringTest("14583.1832", 14583.1832);
  DoubleToStringTest("-14583.1832", -14583.1832);
  DoubleToStringTest("1.23e+25", 1.23e25);
  DoubleToStringTest("1.23e-25", 1.23e-25);
  DoubleToStringTest("5e+25", 5e+25);
  DoubleToStringTest("-5e+25", -5e+25);

  DoubleToStringTest("0", 0);
  DoubleToStringTest("12384", 12384);
  DoubleToStringTest("-12384", -12384);
}

/// numberToStringTest above covers numberToStringSlowPath. This covers
/// numberToString, whose job is to pick between the fast and the slow path.
TEST(ConversionsTest, numberToStringFastPathTest) {
  char buf[NUMBER_TO_STRING_BUF_SIZE];

#define NumberToStringTest(expected, value) \
  EXPECT_EQ(expected, numberToString(value, buf, sizeof(buf)).str())

  // Positive integers below 2^31 take the fast path.
  NumberToStringTest("1", 1);
  NumberToStringTest("9", 9);
  NumberToStringTest("10", 10);
  NumberToStringTest("12384", 12384);
  NumberToStringTest("2147483647", 2147483647);

  // Everything else must fall through to the slow path. Note that zero and -0
  // are excluded by the "> 0" test rather than by an explicit check, so -0 in
  // particular is worth pinning: it must not come out as "-0".
  NumberToStringTest("0", 0.0);
  NumberToStringTest("0", -0.0);
  NumberToStringTest("-1", -1);
  NumberToStringTest("-12384", -12384);
  NumberToStringTest("2147483648", 2147483648.0);
  NumberToStringTest("1.5", 1.5);
  NumberToStringTest("1e+21", 1e21);
  NumberToStringTest("NaN", std::nan(""));
  NumberToStringTest("Infinity", std::numeric_limits<double>::infinity());
  NumberToStringTest("-Infinity", -std::numeric_limits<double>::infinity());

#undef NumberToStringTest
}

} // end anonymous namespace

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/BigIntPrimitive.h"

#include "VMRuntimeTestHelpers.h"
#include "hermes/Support/BigIntTestHelpers.h"

#include <array>
#include <vector>

#include "gtest/gtest.h"

using namespace hermes::bigint;
using namespace hermes::vm;
using namespace testing;

namespace {

using BigIntPrimitiveTest = RuntimeTestFixture;

LeftToRightVector fullRawData(BigIntPrimitive *bigint) {
  auto fullBytes = bigint->getRawDataFull();
  LeftToRightVector ret;
  ret.data.insert(ret.data.end(), fullBytes.begin(), fullBytes.end());
  return ret;
}

LeftToRightVector compactRawData(BigIntPrimitive *bigint) {
  auto fullBytes = bigint->getRawDataCompact();
  LeftToRightVector ret;
  ret.data.insert(ret.data.end(), fullBytes.begin(), fullBytes.end());
  return ret;
}

LeftToRightVector fullRawData(const Handle<BigIntPrimitive> &bigint) {
  return fullRawData(bigint.get());
}

LeftToRightVector compactRawData(const Handle<BigIntPrimitive> &bigint) {
  return compactRawData(bigint.get());
}

LeftToRightVector fullRawData(HermesValue bigint) {
  return fullRawData(bigint.getBigInt());
}

LeftToRightVector compactRawData(HermesValue bigint) {
  return compactRawData(bigint.getBigInt());
}

// Zero is an important corner case -- it has no trailing digits, and it is
// always expected to be non-negative. Thus, it is tested separately.
TEST_F(BigIntPrimitiveTest, CreateZero) {
  // Must create the correct zero from a signed 0.
  auto H0 = fromSignedNoThrowForTest(runtime, 0);
  EXPECT_FALSE(H0->sign());
  EXPECT_EQ(compactRawData(H0), noDigits());
  EXPECT_EQ(fullRawData(H0), noDigits());

  // Must create the correct zero from the canonical (i.e., empty) digit
  // sequence.
  H0 = fromBytesNoThrowForTest(runtime, noDigits());
  EXPECT_FALSE(H0->sign());
  EXPECT_EQ(compactRawData(H0), noDigits());
  EXPECT_EQ(fullRawData(H0), noDigits());

  // Must create the correct zero from the non-canonical (i.e., non-empty)
  // digit sequence.
  H0 = fromBytesNoThrowForTest(runtime, digit(0));
  EXPECT_FALSE(H0->sign());
  EXPECT_EQ(compactRawData(H0), noDigits());
  EXPECT_EQ(fullRawData(H0), noDigits());

  auto digitZero = digit(0, 0, 0, 0, 0, 0, 0, 0);
  auto lotsOfZeros = digitZero + digitZero + digitZero + digitZero + digitZero +
      digitZero + digitZero + digitZero;
  H0 = fromBytesNoThrowForTest(runtime, lotsOfZeros);
  EXPECT_FALSE(H0->sign());
  EXPECT_EQ(compactRawData(H0), noDigits());
  EXPECT_EQ(fullRawData(H0), noDigits());
}

TEST_F(BigIntPrimitiveTest, Create) {
  // Need to be able to create from signed.
  auto HNeg1 = fromSignedNoThrowForTest(runtime, -1);
  EXPECT_TRUE(HNeg1->sign());
  EXPECT_EQ(compactRawData(HNeg1), digit(0xff));
  EXPECT_EQ(
      fullRawData(HNeg1),
      digit(0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff));

  auto H1 = fromSignedNoThrowForTest(runtime, 1);
  EXPECT_FALSE(H1->sign());
  EXPECT_EQ(compactRawData(H1), digit(1));
  EXPECT_EQ(fullRawData(H1), digit(0, 0, 0, 0, 0, 0, 0, 1));

  auto HNeg1234568 = fromSignedNoThrowForTest(runtime, -1234568);
  EXPECT_TRUE(HNeg1234568->sign());
  EXPECT_EQ(compactRawData(HNeg1234568), digit(0xed, 0x29, 0x78));
  EXPECT_EQ(
      fullRawData(HNeg1234568),
      digit(0xff, 0xff, 0xff, 0xff, 0xff, 0xed, 0x29, 0x78));

  // Need to be able to create from short byte array.
  auto HHfe = fromBytesNoThrowForTest(runtime, digit(0xfe));
  EXPECT_TRUE(HHfe->sign());
  EXPECT_EQ(compactRawData(HHfe), digit(0xfe));
  EXPECT_EQ(
      fullRawData(HHfe), digit(0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe));

  auto HH4e = fromBytesNoThrowForTest(runtime, digit(0x4e));
  EXPECT_FALSE(HH4e->sign());
  EXPECT_EQ(compactRawData(HH4e), digit(0x4e));
  EXPECT_EQ(fullRawData(HH4e), digit(0, 0, 0, 0, 0, 0, 0, 0x4e));

  auto HH8000 = fromBytesNoThrowForTest(runtime, digit(0x80, 0x00));
  EXPECT_TRUE(HH8000->sign());
  EXPECT_EQ(compactRawData(HH8000), digit(0x80, 0x00));
  EXPECT_EQ(
      fullRawData(HH8000),
      digit(0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x80, 0x00));

  auto HH4000 = fromBytesNoThrowForTest(runtime, digit(0x40, 0x00));
  EXPECT_FALSE(HH4000->sign());
  EXPECT_EQ(compactRawData(HH4000), digit(0x40, 0x00));
  EXPECT_EQ(fullRawData(HH4000), digit(0, 0, 0, 0, 0, 0, 0x40, 0x00));

  // Need to be able to create from array that's as long as a digit without
  // adding extra digits.
  auto HHffffffffffffffff = fromBytesNoThrowForTest(
      runtime, digit(0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff));
  EXPECT_TRUE(HHffffffffffffffff->sign());
  EXPECT_EQ(compactRawData(HHffffffffffffffff), digit(0xff));
  EXPECT_EQ(
      fullRawData(HHffffffffffffffff),
      digit(0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff));

  auto HH7fffffffffffffff = fromBytesNoThrowForTest(
      runtime, digit(0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff));
  EXPECT_FALSE(HH7fffffffffffffff->sign());
  EXPECT_EQ(
      compactRawData(HH7fffffffffffffff),
      digit(0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff));
  EXPECT_EQ(
      fullRawData(HH7fffffffffffffff),
      digit(0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff));

  // Need to be able to create from arrays that are over WordType in size.
  auto HH400000000000000000 = fromBytesNoThrowForTest(
      runtime, digit(0x40) + digit(0, 0, 0, 0, 0, 0, 0, 0));
  EXPECT_FALSE(HH400000000000000000->sign());
  EXPECT_EQ(
      compactRawData(HH400000000000000000),
      digit(0x40) + digit(0, 0, 0, 0, 0, 0, 0, 0));
  EXPECT_EQ(
      fullRawData(HH400000000000000000),
      digit(0, 0, 0, 0, 0, 0, 0, 0x40) + digit(0, 0, 0, 0, 0, 0, 0, 0));

  auto HH800000000000000000 = fromBytesNoThrowForTest(
      runtime, digit(0x80) + digit(0, 0, 0, 0, 0, 0, 0, 0));
  EXPECT_TRUE(HH800000000000000000->sign());
  EXPECT_EQ(
      compactRawData(HH800000000000000000),
      digit(0x80) + digit(0, 0, 0, 0, 0, 0, 0, 0));
  EXPECT_EQ(
      fullRawData(HH800000000000000000),
      digit(0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x80) +
          digit(0, 0, 0, 0, 0, 0, 0, 0));
}

TEST_F(BigIntPrimitiveTest, FromDouble) {
  auto HMaxDouble = runtime.ignoreAllocationFailure(
      BigIntPrimitive::fromDouble(runtime, std::numeric_limits<double>::max()));
  EXPECT_EQ(
      fullRawData(HMaxDouble),
      digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf8, 0x00) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0));
  EXPECT_EQ(
      compactRawData(HMaxDouble),
      digit(0) + digit(0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf8, 0x00) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0));

  auto HNegMaxDouble = runtime.ignoreAllocationFailure(
      BigIntPrimitive::fromDouble(
          runtime, -std::numeric_limits<double>::max()));
  EXPECT_EQ(
      fullRawData(HNegMaxDouble),
      digit(0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff) +
          digit(0, 0, 0, 0, 0, 0, 0x08, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0));
  EXPECT_EQ(
      compactRawData(HNegMaxDouble),
      digit(0xff) + digit(0, 0, 0, 0, 0, 0, 0x08, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0));

  auto H1024e32 = runtime.ignoreAllocationFailure(
      BigIntPrimitive::fromDouble(runtime, 1024e32));
  EXPECT_EQ(
      fullRawData(H1024e32),
      digit(0x00, 0x13, 0xb8, 0xb5, 0xb5, 0x05, 0x6e, 0x17) +
          digit(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00));
  EXPECT_EQ(
      compactRawData(H1024e32),
      digit(0x13, 0xb8, 0xb5, 0xb5, 0x05, 0x6e, 0x17) +
          digit(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00));

  auto HNeg123456e150 = runtime.ignoreAllocationFailure(
      BigIntPrimitive::fromDouble(runtime, -123456e150));
  EXPECT_EQ(
      fullRawData(HNeg123456e150),
      digit(0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf6) +
          digit(0xca, 0xcf, 0xa4, 0x60, 0x2c, 0x09, 0x00, 0x00) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0));
  EXPECT_EQ(
      compactRawData(HNeg123456e150),
      digit(0xf6) + digit(0xca, 0xcf, 0xa4, 0x60, 0x2c, 0x09, 0x00, 0x00) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0) + digit(0, 0, 0, 0, 0, 0, 0, 0) +
          digit(0, 0, 0, 0, 0, 0, 0, 0));
}

TEST_F(BigIntPrimitiveTest, Compare) {
  auto H0 = fromSignedNoThrowForTest(runtime, 0);
  auto H1 = fromSignedNoThrowForTest(runtime, 1);
  auto HM1 = fromSignedNoThrowForTest(runtime, -1);
  auto HH10000000000000000 = fromBytesNoThrowForTest(
      runtime, digit(1) + digit(0, 0, 0, 0, 0, 0, 0, 0));
  auto HH80000000000000000 = fromBytesNoThrowForTest(
      runtime, digit(0x80) + digit(0, 0, 0, 0, 0, 0, 0, 0));

  // H0 == H0
  EXPECT_EQ(H0->compare(H0.get()), 0);

  // H1 == H1
  EXPECT_EQ(H1->compare(H1.get()), 0);

  // HM1 == HM1
  EXPECT_EQ(HM1->compare(HM1.get()), 0);

  // HH10000000000000000 == HH10000000000000000
  EXPECT_EQ(HH10000000000000000->compare(HH10000000000000000.get()), 0);

  // HH80000000000000000 == HH80000000000000000
  EXPECT_EQ(HH80000000000000000->compare(HH80000000000000000.get()), 0);

  // HH80000000000000000 < HM1 < H0 < H1 < HH10000000000000000
  EXPECT_LT(HH80000000000000000->compare(HM1.get()), 0);
  EXPECT_LT(HH80000000000000000->compare(H0.get()), 0);
  EXPECT_LT(HH80000000000000000->compare(H1.get()), 0);
  EXPECT_LT(HH80000000000000000->compare(HH10000000000000000.get()), 0);

  EXPECT_LT(HM1->compare(H0.get()), 0);
  EXPECT_LT(HM1->compare(H1.get()), 0);
  EXPECT_LT(HM1->compare(HH10000000000000000.get()), 0);

  EXPECT_LT(H0->compare(H1.get()), 0);
  EXPECT_LT(H0->compare(HH10000000000000000.get()), 0);

  EXPECT_LT(H1->compare(HH10000000000000000.get()), 0);

  // HH10000000000000000 > H1 > H0 > HM1 >> HH80000000000000000
  EXPECT_GT(HH10000000000000000->compare(H1.get()), 0);
  EXPECT_GT(HH10000000000000000->compare(H0.get()), 0);
  EXPECT_GT(HH10000000000000000->compare(HM1.get()), 0);
  EXPECT_GT(HH10000000000000000->compare(HH80000000000000000.get()), 0);

  EXPECT_GT(H1->compare(H0.get()), 0);
  EXPECT_GT(H1->compare(HM1.get()), 0);
  EXPECT_GT(H1->compare(HH80000000000000000.get()), 0);

  EXPECT_GT(H0->compare(HM1.get()), 0);
  EXPECT_GT(H0->compare(HH80000000000000000.get()), 0);

  EXPECT_GT(HM1->compare(HH80000000000000000.get()), 0);
}

TEST_F(BigIntPrimitiveTest, CompareSigned) {
  auto H0 = fromSignedNoThrowForTest(runtime, 0);
  auto H1 = fromSignedNoThrowForTest(runtime, 1);
  auto HM1 = fromSignedNoThrowForTest(runtime, -1);
  auto HH7fffffffffffffff = fromBytesNoThrowForTest(
      runtime, digit(0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff));
  auto HH8000000000000000 =
      fromBytesNoThrowForTest(runtime, digit(0x80, 0, 0, 0, 0, 0, 0, 0));

  // 0n == 0
  EXPECT_EQ(H0->compare(0), 0);

  // 1n == 1
  EXPECT_EQ(H1->compare(1), 0);

  // -1n == -1
  EXPECT_EQ(HM1->compare(-1), 0);

  // 0x7fffffffffffffffn == 0x7fffffffffffffff
  EXPECT_EQ(HH7fffffffffffffff->compare(0x7fffffffffffffffll), 0);

  // 0x8000000000000000n == -9223372036854775807
  EXPECT_EQ(
      HH8000000000000000->compare(static_cast<int64_t>(0x8000000000000000ll)),
      0);

  // 0 < 1
  EXPECT_LT(H0->compare(1), 0);

  // 0 > -1
  EXPECT_GT(H0->compare(-1), 0);

  // 0x7fffffffffffffffn > 0x8000000000000000
  EXPECT_GT(
      HH7fffffffffffffff->compare(static_cast<int64_t>(0x8000000000000000ll)),
      0);

  // 0x8000000000000000n < 0x7fffffffffffffff
  EXPECT_LT(HH8000000000000000->compare(0x7fffffffffffffffll), 0);

  // 0x00_0000000000000000_8000000000000000_0000000000000000n
  auto a = fromBytesNoThrowForTest(
      runtime,
      digit(0x00) + digit(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00) +
          digit(0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00) +
          digit(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00));

  // 0x80n
  auto b = fromBytesNoThrowForTest(runtime, digit(0x80));

  // 0x00_0000000000000000_8000000000000000_0000000000000000n > 0
  EXPECT_GT(a->compare(0), 0);

  // 0x80n < 0
  EXPECT_LT(b->compare(0), 0);
}
} // namespace

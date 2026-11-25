/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/HermesValue.h"

#include "VMRuntimeTestHelpers.h"
#include "hermes/Support/Conversions.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringRefUtils.h"

#include <cfloat>
#include <climits>
#include <random>

#include "gtest/gtest.h"

using namespace hermes::vm;

namespace {

[[maybe_unused]] int32_t getRandomInt32(int32_t lower, int32_t upper) {
  std::random_device r;
  std::default_random_engine e1(r());
  std::uniform_int_distribution<int64_t> uniform_dist(lower, upper);
  return uniform_dist(e1);
}

[[maybe_unused]] uint64_t getRandomUInt64(uint64_t lower, uint64_t upper) {
  std::random_device r;
  std::default_random_engine e1(r());
  std::uniform_int_distribution<uint64_t> uniform_dist(lower, upper);
  return uniform_dist(e1);
}

[[maybe_unused]] double getRandomDouble(double lower, double upper) {
  std::random_device r;
  std::default_random_engine e1(r());
  std::uniform_real_distribution<double> uniform_dist(lower, upper);
  return uniform_dist(e1);
}

/// Test the tags that are set in constexpr functions.
TEST(HermesValueTest, ConstexprTagsTest) {
  ASSERT_TRUE(HermesValue::encodeUndefinedValue().isUndefined());
  ASSERT_TRUE(HermesValue::encodeNullValue().isNull());
  ASSERT_TRUE(HermesValue::encodeEmptyValue().isEmpty());
}

using HermesValueRuntimeTest = RuntimeTestFixture;

#if HERMESVM_SANITIZE_HANDLES == 0
TEST_F(HermesValueRuntimeTest, SimpleSmokeTest) {
  // Encode the null value.
  {
    auto V = HermesValue::encodeNullValue();
    EXPECT_TRUE(V.isNull());
    EXPECT_FALSE(V.isUndefined());
    EXPECT_FALSE(V.isDouble());
    EXPECT_FALSE(V.isObject());
    EXPECT_FALSE(V.isBigInt());
    EXPECT_FALSE(V.isString());
    EXPECT_FALSE(V.isBool());
    EXPECT_FALSE(V.isPointer());
    EXPECT_FALSE(V.isNumber());
    EXPECT_FALSE(V.isSymbol());
  }

  // Encode the undefined value.
  {
    auto V = HermesValue::encodeUndefinedValue();
    EXPECT_TRUE(V.isUndefined());
    EXPECT_FALSE(V.isNull());
    EXPECT_FALSE(V.isDouble());
    EXPECT_FALSE(V.isObject());
    EXPECT_FALSE(V.isBigInt());
    EXPECT_FALSE(V.isString());
    EXPECT_FALSE(V.isBool());
    EXPECT_FALSE(V.isPointer());
    EXPECT_FALSE(V.isNumber());
    EXPECT_FALSE(V.isSymbol());
  }

  // Encode the empty value.
  {
    auto V = HermesValue::encodeEmptyValue();
    EXPECT_TRUE(V.isEmpty());
    EXPECT_FALSE(V.isUndefined());
    EXPECT_FALSE(V.isNull());
    EXPECT_FALSE(V.isDouble());
    EXPECT_FALSE(V.isObject());
    EXPECT_FALSE(V.isBigInt());
    EXPECT_FALSE(V.isString());
    EXPECT_FALSE(V.isBool());
    EXPECT_FALSE(V.isPointer());
    EXPECT_FALSE(V.isNumber());
    EXPECT_FALSE(V.isSymbol());
  }

  // Encode "object" by generating random pointers.
  for (int i = 0; i < 1000; i++) {
    uint64_t limit = sizeof(void *) == 4 ? 0xffffffffL : 0x00007fffffffffff;
    auto address = getRandomUInt64(0, limit);
    auto V = HermesValue::encodeObjectValue(reinterpret_cast<void *>(address));
    EXPECT_TRUE(V.isObject());
    EXPECT_FALSE(V.isUndefined());
    EXPECT_FALSE(V.isNull());
    EXPECT_FALSE(V.isDouble());
    EXPECT_FALSE(V.isString());
    EXPECT_FALSE(V.isBool());
    EXPECT_FALSE(V.isNumber());
    EXPECT_FALSE(V.isSymbol());
    EXPECT_TRUE(V.isPointer());
    EXPECT_EQ(0x00007fffffffffff & V.getRaw(), address);

    auto address2 = getRandomUInt64(0, limit);
    auto V2 = V.updatePointer(reinterpret_cast<void *>(address2));
    EXPECT_TRUE(V2.isObject());
    EXPECT_FALSE(V2.isUndefined());
    EXPECT_FALSE(V2.isNull());
    EXPECT_FALSE(V2.isDouble());
    EXPECT_FALSE(V.isBigInt());
    EXPECT_FALSE(V2.isString());
    EXPECT_FALSE(V.isBool());
    EXPECT_FALSE(V.isNumber());
    EXPECT_FALSE(V.isSymbol());
    EXPECT_TRUE(V2.isPointer());
    EXPECT_EQ(0x00007fffffffffff & V2.getRaw(), address2);
  }

  // Encode doubles.
  for (int i = 0; i < 1000; i++) {
    // Check that we can encode the whole range of doubles.
    double value = getRandomDouble(DBL_MIN, DBL_MAX);
    auto V = HermesValue::encodeTrustedNumberValue(value);
    EXPECT_TRUE(V.isDouble());
    EXPECT_TRUE(V.isNumber());
    EXPECT_FALSE(V.isUndefined());
    EXPECT_FALSE(V.isObject());
    EXPECT_FALSE(V.isBigInt());
    EXPECT_FALSE(V.isString());
    EXPECT_FALSE(V.isBool());
    EXPECT_FALSE(V.isNull());
    EXPECT_FALSE(V.isPointer());
    EXPECT_FALSE(V.isSymbol());
    EXPECT_EQ(V.getDouble(), value);
    EXPECT_EQ(V.getNumber(), value);
  }

  // Encode small ints.
  for (int i = 0; i < 1000; i++) {
    // Check that we can encode the whole range of ints.
    int32_t value = getRandomInt32(INT32_MIN, INT32_MAX);
    auto V = HermesValue::encodeTrustedNumberValue(value);
    EXPECT_TRUE(V.isNumber());
    EXPECT_TRUE(V.isDouble());
    EXPECT_FALSE(V.isUndefined());
    EXPECT_FALSE(V.isObject());
    EXPECT_FALSE(V.isBigInt());
    EXPECT_FALSE(V.isString());
    EXPECT_FALSE(V.isNull());
    EXPECT_FALSE(V.isBool());
    EXPECT_FALSE(V.isPointer());
    EXPECT_FALSE(V.isSymbol());
    EXPECT_EQ(V.getNumberAs<int32_t>(), value);
    EXPECT_EQ(V.getNumber(), value);
  }

  // Encode bools.
  {
    auto T = HermesValue::encodeBoolValue(true);
    EXPECT_TRUE(T.isBool());
    EXPECT_FALSE(T.isDouble());
    EXPECT_FALSE(T.isUndefined());
    EXPECT_FALSE(T.isObject());
    EXPECT_FALSE(T.isBigInt());
    EXPECT_FALSE(T.isString());
    EXPECT_FALSE(T.isNull());
    EXPECT_FALSE(T.isPointer());
    EXPECT_FALSE(T.isNumber());
    EXPECT_FALSE(T.isSymbol());
    EXPECT_TRUE(T.getBool());
    auto F = HermesValue::encodeBoolValue(false);
    EXPECT_TRUE(F.isBool());
    EXPECT_FALSE(F.isDouble());
    EXPECT_FALSE(F.isUndefined());
    EXPECT_FALSE(F.isObject());
    EXPECT_FALSE(F.isBigInt());
    EXPECT_FALSE(F.isString());
    EXPECT_FALSE(F.isNull());
    EXPECT_FALSE(F.isPointer());
    EXPECT_FALSE(F.getBool());
    EXPECT_FALSE(F.isNumber());
    EXPECT_FALSE(F.isSymbol());

    EXPECT_FALSE(T == F);
    EXPECT_TRUE(T == T);
  }

  /// Encode strings.
  {
    auto message = createNoThrowForTest(runtime, createUTF16Ref(u"hello"));
    auto V = HermesValue::encodeStringValue(message.get());
    EXPECT_TRUE(V.isString());
    EXPECT_FALSE(V.isBool());
    EXPECT_FALSE(V.isObject());
    EXPECT_FALSE(V.isBigInt());
    EXPECT_FALSE(V.isDouble());
    EXPECT_FALSE(V.isNull());
    EXPECT_FALSE(V.isUndefined());
    EXPECT_FALSE(V.isNumber());
    EXPECT_FALSE(V.isSymbol());
    EXPECT_TRUE(V.isPointer());
    EXPECT_EQ(V.getPointer(), message.get());

    auto message2 = createNoThrowForTest(runtime, createUTF16Ref(u"hi"));
    auto V2 = HermesValue::encodeStringValue(message2.get());
    EXPECT_TRUE(V2.isString());
    EXPECT_FALSE(V2.isBool());
    EXPECT_FALSE(V2.isObject());
    EXPECT_FALSE(V.isBigInt());
    EXPECT_FALSE(V2.isDouble());
    EXPECT_FALSE(V2.isNull());
    EXPECT_FALSE(V2.isUndefined());
    EXPECT_FALSE(V2.isNumber());
    EXPECT_FALSE(V.isSymbol());
    EXPECT_TRUE(V2.isPointer());
    EXPECT_EQ(V2.getPointer(), message2.get());

    EXPECT_TRUE(V == V);
    EXPECT_TRUE(V2 == V2);
    EXPECT_FALSE(V == V2);
  }

  /// Encode bigints.
  {
    auto H = fromSignedNoThrowForTest(runtime, 0);
    auto V = HermesValue::encodeBigIntValue(H.get());
    EXPECT_TRUE(V.isBigInt());
    EXPECT_FALSE(V.isBool());
    EXPECT_FALSE(V.isObject());
    EXPECT_FALSE(V.isString());
    EXPECT_FALSE(V.isDouble());
    EXPECT_FALSE(V.isNull());
    EXPECT_FALSE(V.isUndefined());
    EXPECT_FALSE(V.isNumber());
    EXPECT_FALSE(V.isSymbol());
    EXPECT_TRUE(V.isPointer());
    EXPECT_EQ(V.getPointer(), H.get());

    auto H2 = fromSignedNoThrowForTest(runtime, 0);
    auto V2 = HermesValue::encodeBigIntValue(H2.get());
    EXPECT_TRUE(V2.isBigInt());
    EXPECT_FALSE(V2.isBool());
    EXPECT_FALSE(V2.isObject());
    EXPECT_FALSE(V2.isString());
    EXPECT_FALSE(V2.isDouble());
    EXPECT_FALSE(V2.isNull());
    EXPECT_FALSE(V2.isUndefined());
    EXPECT_FALSE(V2.isNumber());
    EXPECT_FALSE(V.isSymbol());
    EXPECT_TRUE(V2.isPointer());
    EXPECT_EQ(V2.getPointer(), H2.get());

    EXPECT_TRUE(V == V);
    EXPECT_TRUE(V2 == V2);
  }

  // Encode symbols.
  for (int i = 0; i < 1000; i++) {
    // Check that we can encode the whole range of ints.
    SymbolID value = SymbolID::unsafeCreate(
        (uint32_t)getRandomUInt64(0, SymbolID::LAST_INVALID_ID - 1));
    auto V = HermesValue::encodeSymbolValue(value);
    EXPECT_FALSE(V.isNumber());
    EXPECT_FALSE(V.isDouble());
    EXPECT_FALSE(V.isUndefined());
    EXPECT_FALSE(V.isObject());
    EXPECT_FALSE(V.isBigInt());
    EXPECT_FALSE(V.isString());
    EXPECT_FALSE(V.isNull());
    EXPECT_FALSE(V.isBool());
    EXPECT_FALSE(V.isPointer());
    EXPECT_TRUE(V.isSymbol());
    EXPECT_EQ(value, V.getSymbol());
  }
}
#endif

TEST(HermesValueTest, NanTest) {
  double v1 = HermesValue::encodeNaNValue().getDouble();
  double v2 = HermesValue::encodeTrustedNumberValue(
                  std::numeric_limits<double>::quiet_NaN())
                  .getDouble();
  EXPECT_TRUE(std::isnan(v1));
  EXPECT_TRUE(std::isnan(v2));
  int64_t v1_int = llvh::DoubleToBits(v1);
  int64_t v2_int = llvh::DoubleToBits(v2);
  EXPECT_EQ(v1_int, v2_int);
}

TEST(HermesValueTest, HVConstantsTest) {
  auto A = HVConstants::kZero;
  auto B = HVConstants::kOne;
  auto C = HVConstants::kNegOne;
  auto D = HermesValue::encodeTrustedNumberValue(0);
  auto E = HermesValue::encodeTrustedNumberValue(1);
  auto F = HermesValue::encodeTrustedNumberValue(-1);

  EXPECT_TRUE(A == D);
  EXPECT_TRUE(B == E);
  EXPECT_TRUE(C == F);
}

TEST(HermesValueTest, EqualityTest) {
  auto A = HermesValue::encodeNullValue();
  auto B = HermesValue::encodeUndefinedValue();
  auto C = HermesValue::encodeUndefinedValue();
  auto D = HermesValue::encodeTrustedNumberValue(1.23);
  auto E = HermesValue::encodeTrustedNumberValue(1.23);
  auto F = HermesValue::encodeTrustedNumberValue(274);
  auto G = HermesValue::encodeTrustedNumberValue(274);

  EXPECT_TRUE(A == A);
  EXPECT_FALSE(A == B);
  EXPECT_TRUE(B == C);
  EXPECT_FALSE(C == D);
  EXPECT_TRUE(D == E);
  EXPECT_FALSE(E == F);
  EXPECT_TRUE(F == G);
}

TEST(HermesValueTest, OutputStreamTest) {
  std::string result;
  llvh::raw_string_ostream OS(result);

  result.clear();
  OS << HermesValue::encodeObjectValueUnsafe(nullptr);
  EXPECT_EQ("[Object :0 0x00000000]", OS.str());

  result.clear();
  OS << HermesValue::encodeBigIntValueUnsafe(nullptr);
  EXPECT_EQ("[BigInt :0 0x00000000]", OS.str());

  result.clear();
  OS << HermesValue::encodeStringValueUnsafe(nullptr);
  EXPECT_EQ("[String :0 0x00000000]", OS.str());

  result.clear();
  OS << HermesValue::encodeTrustedNumberValue(123);
  EXPECT_EQ("[double 123]", OS.str());

  result.clear();
  OS << HermesValue::encodeBoolValue(true);
  EXPECT_EQ("true", OS.str());

  result.clear();
  OS << HermesValue::encodeBoolValue(false);
  EXPECT_EQ("false", OS.str());

  result.clear();
  OS << HermesValue::encodeNullValue();
  EXPECT_EQ("null", OS.str());

  result.clear();
  OS << HermesValue::encodeUndefinedValue();
  EXPECT_EQ("undefined", OS.str());

  result.clear();
  OS << HermesValue::encodeTrustedNumberValue(1.23);
  EXPECT_EQ("[double 1.230000e+00]", OS.str());

  result.clear();
  OS << HermesValue::encodeSymbolValue(SymbolID::unsafeCreate(256));
  EXPECT_EQ("[Symbol (Internal) 256]", OS.str());
}

TEST(HermesValueTest, NativePointerTest) {
#if LLVM_PTR_SIZE == 8
  // Ensure that native 64-bit pointers are not incorrectly sign extended. Bit
  // 47 is the one controlling the sign and all user pointers must have it
  // cleared.
  void *scaryPointer = reinterpret_cast<void *>((uint64_t)1 << 46);
  auto hv = HermesValue::encodeNativePointer(scaryPointer);
  ASSERT_EQ(scaryPointer, hv.getNativePointer<void>());

  // Ensure that MTE bits set in the top byte of a native pointer are not lost.
  void *mtePointer = reinterpret_cast<void *>((7ULL << 56) | 0xFACEB00C);
  auto mteHV = HermesValue::encodeNativePointer(mtePointer);
  ASSERT_EQ(mtePointer, mteHV.getNativePointer<void>());
#endif
}
} // anonymous namespace.

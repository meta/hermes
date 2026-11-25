/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/SmallHermesValue.h"

#include "VMRuntimeTestHelpers.h"
#include "hermes/Support/Conversions.h"
#include "hermes/VM/PrimitiveBox.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringRefUtils.h"
#include "hermes/VM/WeakRoot-inline.h"

#include <cfloat>
#include <climits>
#include <random>

#include "gtest/gtest.h"

using namespace hermes::vm;

namespace {

int32_t getRandomInt32(int32_t lower, int32_t upper) {
  std::random_device r;
  std::default_random_engine e1(r());
  std::uniform_int_distribution<int64_t> uniform_dist(lower, upper);
  return uniform_dist(e1);
}

uint64_t getRandomUInt64(uint64_t lower, uint64_t upper) {
  std::random_device r;
  std::default_random_engine e1(r());
  std::uniform_int_distribution<uint64_t> uniform_dist(lower, upper);
  return uniform_dist(e1);
}

double getRandomDouble(double lower, double upper) {
  std::random_device r;
  std::default_random_engine e1(r());
  std::uniform_real_distribution<double> uniform_dist(lower, upper);
  return uniform_dist(e1);
}

using SmallHermesValueRuntimeTest = RuntimeTestFixture;

TEST_F(SmallHermesValueRuntimeTest, NullTest) {
  // Encode the null value.
  auto SHV = SmallHermesValue::encodeHermesValue(
      HermesValue::encodeNullValue(), runtime);
  EXPECT_TRUE(SHV.isNull());
  // Should not be boxed.
  EXPECT_FALSE(SHV.isPointer());
  EXPECT_TRUE(SHV.unboxToHV(runtime).isNull());
}

TEST_F(SmallHermesValueRuntimeTest, UndefinedTest) {
  // Encode the undefined value.
  auto SHV = SmallHermesValue::encodeHermesValue(
      HermesValue::encodeUndefinedValue(), runtime);
  EXPECT_TRUE(SHV.isUndefined());
  // Should not be boxed.
  EXPECT_FALSE(SHV.isPointer());
  auto HV = SHV.unboxToHV(runtime);
  EXPECT_TRUE(HV.isUndefined());
}

TEST_F(SmallHermesValueRuntimeTest, EmptyTest) {
  // Encode the empty value.
  auto SHV = SmallHermesValue::encodeHermesValue(
      HermesValue::encodeEmptyValue(), runtime);
  EXPECT_TRUE(SHV.isEmpty());
  // Should not be boxed.
  EXPECT_FALSE(SHV.isPointer());
  auto HV = SHV.unboxToHV(runtime);
  EXPECT_TRUE(HV.isEmpty());
}

TEST_F(SmallHermesValueRuntimeTest, BoolTest) {
  // Encode true.
  auto TSHV = SmallHermesValue::encodeHermesValue(
      HermesValue::encodeBoolValue(true), runtime);
  EXPECT_FALSE(TSHV.isPointer());
  EXPECT_TRUE(TSHV.isBool());
  EXPECT_TRUE(TSHV.getBool());
  auto THV = TSHV.unboxToHV(runtime);
  EXPECT_TRUE(THV.isBool());
  EXPECT_TRUE(THV.getBool());

  // Encode false.
  auto FSHV = SmallHermesValue::encodeHermesValue(
      HermesValue::encodeBoolValue(false), runtime);
  EXPECT_FALSE(FSHV.isPointer());
  EXPECT_TRUE(FSHV.isBool());
  EXPECT_FALSE(FSHV.getBool());
  auto FHV = FSHV.unboxToHV(runtime);
  EXPECT_TRUE(FHV.isBool());
  EXPECT_FALSE(FHV.getBool());
}

TEST_F(SmallHermesValueRuntimeTest, DoubleTest) {
  // Encode doubles.
  auto verifyDouble = [&](double d) {
    auto SHV = SmallHermesValue::encodeHermesValue(
        HermesValue::encodeTrustedNumberValue(d), runtime);
    auto HV = SHV.unboxToHV(runtime);
    EXPECT_TRUE(HV.isNumber()) << "double not encoded as number: " << d;
    // Check for bitwise equality so we can test things like NaN and -0.
    EXPECT_EQ(llvh::DoubleToBits(HV.getNumber()), llvh::DoubleToBits(d))
        << "doubles not the same: " << d;
  };

  verifyDouble(std::numeric_limits<double>::quiet_NaN());
  verifyDouble(std::numeric_limits<double>::min());
  verifyDouble(std::numeric_limits<double>::max());
  verifyDouble(std::numeric_limits<double>::epsilon());
  verifyDouble(-0.0);

  // Try some random doubles.
  for (int i = 0; i < 1000; i++)
    verifyDouble(getRandomDouble(DBL_MIN, DBL_MAX));
}

TEST_F(SmallHermesValueRuntimeTest, SmiTest) {
  // Encode SMIs.
  auto verifySmi = [&](int32_t i) {
    auto SHV = SmallHermesValue::encodeHermesValue(
        HermesValue::encodeTrustedNumberValue(i), runtime);
// When Handle-SAN is enabled, we put all numbers on the heap.
#if HERMESVM_SANITIZE_HANDLES == 0
    EXPECT_FALSE(SHV.isPointer());
#endif
    auto HV = SHV.unboxToHV(runtime);
    EXPECT_TRUE(HV.isNumber()) << "SMI not encoded as number: " << i;
    EXPECT_EQ(HV.getNumber(), i);
  };

  verifySmi(0);
  // We can do min int values for large bit widths, but only because they're
  // powers of 2 -- their mantissa has trailing zeros.
  verifySmi(llvh::minIntN(40));
  // For positive max values, which are all ones, the max we we can do is 19:
  // The max N-bit integer has N-1 ones.  So that's 18 ones.
  // One of these is an implicit leading 1 in floating point.  So 17 ones.
  // 32 - (3 tag bits) - (1 sign bit) - (11 exponent bits) = 17;
  verifySmi(llvh::maxIntN(19));
  // Try some random SMIs (in the 19-bit range described above.
  for (int i = 0; i < 1000; i++)
    verifySmi(getRandomInt32(llvh::minIntN(19), llvh::maxIntN(19)));
}

TEST_F(SmallHermesValueRuntimeTest, EncodeNumberTest) {
  // Encode SMIs.
  auto verifySmi = [&](int64_t i) {
    double d = static_cast<double>(i);
    auto SHV = SmallHermesValue::encodeNumberValue(d, runtime);
// When Handle-SAN is enabled, we put all numbers on the heap.
#if HERMESVM_SANITIZE_HANDLES == 0
    EXPECT_FALSE(SHV.isPointer());
#endif
    auto HV = SHV.unboxToHV(runtime);
    EXPECT_TRUE(HV.isNumber()) << "SMI not encoded as number: " << i;
    EXPECT_EQ(HV.getNumber(), d);
  };

  verifySmi(0);
  // We can do min int values for large bit widths, but only because they're
  // powers of 2 -- their mantissa has trailing zeros.
  verifySmi(llvh::minIntN(40));
  // For positive max values, which are all ones, the max we we can do is 19:
  // The max N-bit integer has N-1 ones.  So that's 18 ones.
  // One of these is an implicit leading 1 in floating point.  So 17 ones.
  // 32 - (3 tag bits) - (1 sign bit) - (11 exponent bits) = 17;
  verifySmi(llvh::maxIntN(19));
  // Try some random SMIs (in the 19-bit range described above.
  for (int i = 0; i < 1000; i++)
    verifySmi(getRandomInt32(llvh::minIntN(19), llvh::maxIntN(19)));
}

TEST_F(SmallHermesValueRuntimeTest, SymbolTest) {
  auto verifySymbol = [&](uint32_t id) {
    auto sym = SymbolID::unsafeCreate(id);
    auto SHV = SmallHermesValue::encodeHermesValue(
        HermesValue::encodeSymbolValue(sym), runtime);
    EXPECT_FALSE(SHV.isPointer());
    auto HV = SHV.unboxToHV(runtime);
    EXPECT_TRUE(HV.isSymbol());
    EXPECT_EQ(sym, HV.getSymbol());
  };

  verifySymbol(0);
  verifySymbol(1);
  verifySymbol(2);
  verifySymbol(SymbolID::EMPTY_ID);
  verifySymbol(SymbolID::DELETED_ID);

  // Try some random SymbolIDs.
  for (int i = 0; i < 1000; i++)
    verifySymbol((uint32_t)getRandomUInt64(0, SymbolID::LAST_INVALID_ID));
}

TEST_F(SmallHermesValueRuntimeTest, StringTest) {
  // Encode a string.
  auto message = createNoThrowForTest(runtime, createUTF16Ref(u"hello"));
  auto SHV = SmallHermesValue::encodeHermesValue(
      HermesValue::encodeStringValue(message.get()), runtime);
  EXPECT_TRUE(SHV.isPointer());
  EXPECT_TRUE(SHV.isString());
  auto HV = SHV.unboxToHV(runtime);
  EXPECT_TRUE(HV.isString());
  EXPECT_EQ(HV.getString(), message.get());
}

TEST_F(
    SmallHermesValueRuntimeTest,
    WeakSmallHermesValuePreservesPointerTagsAcrossEvacuation) {
  auto weakValue = std::make_shared<WeakSmallHermesValue>();
  runtime.addCustomWeakRootsFunction(
      [weakValue](GC *, WeakRootAcceptor &acceptor) {
        acceptor.acceptWeak(*weakValue);
      });

  struct : Locals {
    PinnedValue<StringPrimitive> str;
#ifdef HERMESVM_BOXED_DOUBLES
    PinnedValue<SmallHermesValue> boxedDouble;
#endif
  } lv;
  LocalsRAII lraii{runtime, &lv};

  auto strRes =
      StringPrimitive::create(runtime, createUTF16Ref(u"weak string"));
  ASSERT_NE(ExecutionStatus::EXCEPTION, strRes);
  lv.str.castAndSetHermesValue<StringPrimitive>(*strRes);
  weakValue->set(SmallHermesValue::encodeStringValue(*lv.str, runtime));

  runtime.collect("test");

  EXPECT_EQ(
      weakValue->getPointerNoBarrierUnsafe(runtime),
      static_cast<GCCell *>(*lv.str));

  HermesValue value =
      weakValue->unboxToHVWithReadBarrier(runtime, runtime.getHeap());
  ASSERT_TRUE(value.isString());
  EXPECT_EQ(value.getString(), *lv.str);

#ifdef HERMESVM_BOXED_DOUBLES
  constexpr double kBoxedDoubleValue = std::numeric_limits<double>::max();
  lv.boxedDouble =
      SmallHermesValue::encodeNumberValue(kBoxedDoubleValue, runtime);
  ASSERT_TRUE(lv.boxedDouble.getSmallHermesValue().isBoxedDouble());
  weakValue->set(lv.boxedDouble.getSmallHermesValue());

  runtime.collect("test");

  const auto boxedDouble = lv.boxedDouble.getSmallHermesValue();
  ASSERT_TRUE(boxedDouble.isBoxedDouble());
  EXPECT_EQ(
      weakValue->getPointerNoBarrierUnsafe(runtime),
      boxedDouble.getPointer(runtime));
  EXPECT_EQ(weakValue->getRaw(), boxedDouble.getRaw());

  HermesValue boxedDoubleValue =
      weakValue->unboxToHVWithReadBarrier(runtime, runtime.getHeap());
  ASSERT_TRUE(boxedDoubleValue.isNumber());
  EXPECT_EQ(boxedDoubleValue.getNumber(), kBoxedDoubleValue);
#endif
}

TEST_F(SmallHermesValueRuntimeTest, BigIntTest) {
  // Encode a bigint.
  auto H = fromSignedNoThrowForTest(runtime, 0x123);
  auto SHV = SmallHermesValue::encodeHermesValue(
      HermesValue::encodeBigIntValue(H.get()), runtime);
  EXPECT_TRUE(SHV.isPointer());
  EXPECT_TRUE(SHV.isBigInt());
  auto HV = SHV.unboxToHV(runtime);
  EXPECT_TRUE(HV.isBigInt());
  EXPECT_EQ(HV.getBigInt(), H.get());
}

TEST_F(SmallHermesValueRuntimeTest, ObjectTest) {
  // Encode an object.
  auto obj = JSNumber::create(
      runtime, 3.14, Handle<JSObject>::vmcast(&runtime.numberPrototype));
  auto SHV = SmallHermesValue::encodeHermesValue(
      HermesValue::encodeObjectValue(obj.get()), runtime);
  EXPECT_TRUE(SHV.isPointer());
  EXPECT_TRUE(SHV.isObject());
  auto HV = SHV.unboxToHV(runtime);
  EXPECT_TRUE(HV.isObject());
  EXPECT_EQ(HV.getObject(), obj.get());
}

TEST_F(SmallHermesValueRuntimeTest, TryEncodeNoAllocMatchesEncode) {
  struct : Locals {
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii{runtime, &lv};
  auto strRes = StringPrimitive::create(runtime, createUTF16Ref(u"hello"));
  ASSERT_NE(ExecutionStatus::EXCEPTION, strRes);
  lv.str.castAndSetHermesValue<StringPrimitive>(*strRes);

  // Compare the raw bits of the two encodings of \p hv. Neither allocates
  // for a pointer, so a pointer in hv stays valid across both.
  auto expectSame = [&](HermesValue hv, const char *what) {
    hermes::OptValue<SmallHermesValue> tried =
        SmallHermesValue::tryEncodeHermesValueNoAlloc(hv, runtime);
    ASSERT_TRUE(tried.hasValue()) << what;
    EXPECT_EQ(
        tried->getRaw(),
        SmallHermesValue::encodeHermesValue(hv, runtime).getRaw())
        << what;
  };

#if HERMESVM_SANITIZE_HANDLES != 0 && defined(HERMESVM_BOXED_DOUBLES)
  // Under Handle-SAN, HV32 and BOXED box every number, so the encode
  // without allocation must decline even a small integer.
  EXPECT_FALSE(
      SmallHermesValue::tryEncodeHermesValueNoAlloc(
          HermesValue::encodeTrustedNumberValue(42), runtime)
          .hasValue());
#else
  expectSame(HermesValue::encodeTrustedNumberValue(42), "small integer");
#endif
  expectSame(HermesValue::encodeBoolValue(true), "true");
  expectSame(HermesValue::encodeUndefinedValue(), "undefined");
  expectSame(
      HermesValue::encodeSymbolValue(SymbolID::unsafeCreate(42)), "symbol");
  expectSame(HermesValue::encodeStringValue(*lv.str), "string");
}

TEST_F(SmallHermesValueRuntimeTest, TryEncodeNoAllocNoneForBoxed) {
  HermesValue hv = HermesValue::encodeTrustedNumberValue(0.1);
  hermes::OptValue<SmallHermesValue> tried =
      SmallHermesValue::tryEncodeHermesValueNoAlloc(hv, runtime);
#ifdef HERMESVM_BOXED_DOUBLES
  // 0.1 does not fit inline, so encodeHermesValue would box it.
  EXPECT_FALSE(tried.hasValue());
#else
  ASSERT_TRUE(tried.hasValue());
  EXPECT_EQ(
      tried->getRaw(),
      SmallHermesValue::encodeHermesValue(hv, runtime).getRaw());
#endif
}

#ifdef HERMESVM_BOXED_DOUBLES
#ifdef HERMESVM_COMPRESSED_POINTERS
TEST_F(SmallHermesValueRuntimeTest, EncodeDecodeAsHV64) {
  // SHV of compressible non-Number, e.g., Undefined
  {
    SmallHermesValue SHV = SmallHermesValue::encodeUndefinedValue();
    HermesValue HV = SHV.encodeAsHermesValue();
    EXPECT_TRUE(HV.isRawHV32());
    SmallHermesValue decodedSHV = SmallHermesValue::decodeFromHermesValue(HV);
    ASSERT_EQ(SHV.getRaw(), decodedSHV.getRaw());
  }

  // SHV of compressible Number
  {
    SmallHermesValue SHV = SmallHermesValue::encodeNumberValue(0.0, runtime);
#if HERMESVM_SANITIZE_HANDLES == 0
    ASSERT_TRUE(SHV.isInlinedDouble());
#endif
    HermesValue HV = SHV.encodeAsHermesValue();
    EXPECT_TRUE(HV.isRawHV32());
    SmallHermesValue decodedSHV = SmallHermesValue::decodeFromHermesValue(HV);
    ASSERT_EQ(SHV.getRaw(), decodedSHV.getRaw());
  }

  // SHV of non-compressible Number
  {
    double d = std::numeric_limits<double>::max();
    SmallHermesValue SHV = SmallHermesValue::encodeNumberValue(d, runtime);
    ASSERT_TRUE(SHV.isBoxedDouble());
    HermesValue HV = SHV.encodeAsHermesValue();
    EXPECT_TRUE(HV.isRawHV32());
    SmallHermesValue decodedSHV = SmallHermesValue::decodeFromHermesValue(HV);
    ASSERT_EQ(SHV.getRaw(), decodedSHV.getRaw());
  }

  // SHV of Object
  {
    auto obj = JSObject::create(runtime);
    SmallHermesValue SHV =
        SmallHermesValue::encodeObjectValue(obj.get(), runtime);
    HermesValue HV = SHV.encodeAsHermesValue();
    EXPECT_TRUE(HV.isRawHV32());
    SmallHermesValue decodedSHV = SmallHermesValue::decodeFromHermesValue(HV);
    ASSERT_EQ(SHV.getRaw(), decodedSHV.getRaw());
  }

  // SHV of Symbol
  {
    auto sym = SymbolID::unsafeCreate(42);
    SmallHermesValue SHV = SmallHermesValue::encodeSymbolValue(sym);
    HermesValue HV = SHV.encodeAsHermesValue();
    EXPECT_TRUE(HV.isRawHV32());
    SmallHermesValue decodedSHV = SmallHermesValue::decodeFromHermesValue(HV);
    ASSERT_EQ(SHV.getRaw(), decodedSHV.getRaw());
  }

  // SHV of String
  {
    auto str = createNoThrowForTest(runtime, createUTF16Ref(u"I'm a string"));
    SmallHermesValue SHV = SmallHermesValue::encodeStringValue(*str, runtime);
    HermesValue HV = SHV.encodeAsHermesValue();
    EXPECT_TRUE(HV.isRawHV32());
    SmallHermesValue decodedSHV = SmallHermesValue::decodeFromHermesValue(HV);
    ASSERT_EQ(SHV.getRaw(), decodedSHV.getRaw());
  }
}
#else
TEST_F(SmallHermesValueRuntimeTest, EncodeDecodeAsHV64) {
  // SHV of compressible non-Number, e.g., Undefined
  {
    SmallHermesValue SHV = SmallHermesValue::encodeUndefinedValue();
    HermesValue HV = SHV.encodeAsHermesValue();
    EXPECT_TRUE(HV.isUndefined());
    SmallHermesValue decodedSHV = SmallHermesValue::decodeFromHermesValue(HV);
    ASSERT_EQ(SHV.getRaw(), decodedSHV.getRaw());
  }

  // SHV of compressible Number
  {
    SmallHermesValue SHV = SmallHermesValue::encodeNumberValue(0.0, runtime);
    HermesValue HV = SHV.encodeAsHermesValue();
#if HERMESVM_SANITIZE_HANDLES == 0
    ASSERT_TRUE(SHV.isInlinedDouble());
    EXPECT_TRUE(HV.isNumber());
#else
    ASSERT_TRUE(SHV.isBoxedDouble());
    ASSERT_TRUE(HV.isRawHV32());
#endif
    SmallHermesValue decodedSHV = SmallHermesValue::decodeFromHermesValue(HV);
    ASSERT_EQ(SHV.getRaw(), decodedSHV.getRaw());
  }

  // SHV of non-compressible Number
  {
    double d = std::numeric_limits<double>::max();
    SmallHermesValue SHV = SmallHermesValue::encodeNumberValue(d, runtime);
    ASSERT_TRUE(SHV.isBoxedDouble());
    HermesValue HV = SHV.encodeAsHermesValue();
    EXPECT_TRUE(HV.isRawHV32());
    SmallHermesValue decodedSHV = SmallHermesValue::decodeFromHermesValue(HV);
    ASSERT_EQ(SHV.getRaw(), decodedSHV.getRaw());
  }

  // SHV of Object
  {
    auto obj = JSObject::create(runtime);
    SmallHermesValue SHV =
        SmallHermesValue::encodeObjectValue(obj.get(), runtime);
    HermesValue HV = SHV.encodeAsHermesValue();
    EXPECT_TRUE(HV.isRawHV32());
    SmallHermesValue decodedSHV = SmallHermesValue::decodeFromHermesValue(HV);
    ASSERT_EQ(SHV.getRaw(), decodedSHV.getRaw());
  }

  // SHV of Symbol
  {
    auto sym = SymbolID::unsafeCreate(42);
    SmallHermesValue SHV = SmallHermesValue::encodeSymbolValue(sym);
    HermesValue HV = SHV.encodeAsHermesValue();
    EXPECT_TRUE(HV.isRawHV32());
    SmallHermesValue decodedSHV = SmallHermesValue::decodeFromHermesValue(HV);
    ASSERT_EQ(SHV.getRaw(), decodedSHV.getRaw());
  }

  // SHV of String
  {
    auto str = createNoThrowForTest(runtime, createUTF16Ref(u"I'm a string"));
    SmallHermesValue SHV = SmallHermesValue::encodeStringValue(*str, runtime);
    HermesValue HV = SHV.encodeAsHermesValue();
    EXPECT_TRUE(HV.isRawHV32());
    SmallHermesValue decodedSHV = SmallHermesValue::decodeFromHermesValue(HV);
    ASSERT_EQ(SHV.getRaw(), decodedSHV.getRaw());
  }
}
#endif
#endif

} // anonymous namespace.

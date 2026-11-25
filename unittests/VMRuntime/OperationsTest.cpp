/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/Operations.h"

#include "hermes/VM/JSObject.h"
#include "hermes/VM/PrimitiveBox.h"
#include "hermes/VM/StringView.h"

#include "VMRuntimeTestHelpers.h"

#include <limits>

#include "gtest/gtest.h"

using namespace hermes::vm;

namespace {

CallResult<HermesValue> res{ExecutionStatus ::EXCEPTION};
#define IsSameValueTest(result, v1, v2)   \
  {                                       \
    EXPECT_##result(isSameValue(v1, v2)); \
    EXPECT_##result(isSameValue(v2, v1)); \
  }

using OperationsTest = RuntimeTestFixture;

TEST_F(OperationsTest, IsSameValueTest) {
  struct : public Locals {
    PinnedValue<StringPrimitive> s1;
    PinnedValue<StringPrimitive> s2;
    PinnedValue<StringPrimitive> s3;
    PinnedValue<BigIntPrimitive> b1;
    PinnedValue<BigIntPrimitive> b2;
    PinnedValue<BigIntPrimitive> b3;
    PinnedValue<JSObject> obj1;
    PinnedValue<JSObject> obj2;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  PinnedHermesValue v1;
  PinnedHermesValue v2;
  PinnedHermesValue v3;

  v1 = HermesValue::encodeNullValue();
  v2 = HermesValue::encodeNullValue();
  IsSameValueTest(TRUE, v1, v2);

  v1 = HermesValue::encodeUndefinedValue();
  v2 = HermesValue::encodeUndefinedValue();
  IsSameValueTest(TRUE, v1, v2);

  v1 = HermesValue::encodeNullValue();
  v2 = HermesValue::encodeUndefinedValue();
  IsSameValueTest(FALSE, v1, v2);

  v1 = HermesValue::encodeBoolValue(true);
  v2 = HermesValue::encodeBoolValue(true);
  IsSameValueTest(TRUE, v1, v2);

  v1 = HermesValue::encodeBoolValue(true);
  v2 = HermesValue::encodeBoolValue(false);
  IsSameValueTest(FALSE, v1, v2);

  v1 = HermesValue::encodeUndefinedValue();
  v2 = HermesValue::encodeBoolValue(false);
  IsSameValueTest(FALSE, v1, v2);

  v1 = HermesValue::encodeTrustedNumberValue(123.45);
  v2 = HermesValue::encodeTrustedNumberValue(123.45);
  IsSameValueTest(TRUE, v1, v2);

  v1 = HermesValue::encodeTrustedNumberValue(-0.0);
  v2 = HermesValue::encodeTrustedNumberValue(+0.0);
  IsSameValueTest(FALSE, v1, v2);

  v1 = HermesValue::encodeTrustedNumberValue(
      std::numeric_limits<double>::quiet_NaN());
  v2 = HermesValue::encodeTrustedNumberValue(
      std::numeric_limits<double>::quiet_NaN());
  IsSameValueTest(TRUE, v1, v2);

  v1 = HermesValue::encodeTrustedNumberValue(
      std::numeric_limits<double>::infinity());
  v2 = HermesValue::encodeTrustedNumberValue(
      std::numeric_limits<double>::infinity());
  IsSameValueTest(TRUE, v1, v2);

  v1 = HermesValue::encodeTrustedNumberValue(
      std::numeric_limits<double>::infinity());
  v2 = HermesValue::encodeTrustedNumberValue(
      -std::numeric_limits<double>::infinity());
  IsSameValueTest(FALSE, v1, v2);

  v1 = HermesValue::encodeTrustedNumberValue(+0);
  v2 = HermesValue::encodeTrustedNumberValue(+0.0);
  IsSameValueTest(TRUE, v1, v2);

  v1 = HermesValue::encodeTrustedNumberValue(-134);
  v2 = HermesValue::encodeTrustedNumberValue(-134.0);
  IsSameValueTest(TRUE, v1, v2);

  v1 = HermesValue::encodeTrustedNumberValue(182);
  v2 = HermesValue::encodeTrustedNumberValue(182.54);
  IsSameValueTest(FALSE, v1, v2);

  auto nullObj = runtime.makeNullHandle<JSObject>();
  lv.obj1 = JSObject::create(runtime, nullObj);
  lv.obj2 = JSObject::create(runtime, nullObj);

  v1 = HermesValue::encodeObjectValue(lv.obj1.get());
  v2 = HermesValue::encodeObjectValue(lv.obj2.get());
  IsSameValueTest(FALSE, v1, v2);

  v1 = HermesValue::encodeObjectValue(lv.obj1.get());
  v2 = HermesValue::encodeObjectValue(lv.obj1.get());
  IsSameValueTest(TRUE, v1, v2);

  auto s1 =
      createNoThrowForTest(runtime, createUTF16Ref(u"hello world"), lv.s1);
  auto s2 =
      createNoThrowForTest(runtime, createUTF16Ref(u"hello world"), lv.s2);
  auto s3 = createNoThrowForTest(runtime, createUTF16Ref(u"hi earth"), lv.s3);

  v1 = HermesValue::encodeStringValue(s1.get());
  v2 = HermesValue::encodeStringValue(s2.get());
  v3 = HermesValue::encodeStringValue(s3.get());
  IsSameValueTest(TRUE, v1, v2);
  IsSameValueTest(FALSE, v1, v3);

  v1 = HermesValue::encodeSymbolValue(SymbolID::unsafeCreate(1));
  v2 = HermesValue::encodeSymbolValue(SymbolID::unsafeCreate(2));
  IsSameValueTest(FALSE, v1, v2);
  IsSameValueTest(TRUE, v1, v1);

  v2 = HermesValue::encodeBoolValue(true);
  IsSameValueTest(FALSE, v1, v2);

  v2 = HermesValue::encodeTrustedNumberValue(0);
  IsSameValueTest(FALSE, v1, v2);

  auto b1 = fromSignedNoThrowForTest(runtime, 1, lv.b1);
  auto b2 = fromSignedNoThrowForTest(runtime, 1, lv.b2);
  auto b3 = fromSignedNoThrowForTest(runtime, -22, lv.b3);

  v1 = HermesValue::encodeBigIntValue(b1.get());
  v2 = HermesValue::encodeBigIntValue(b2.get());
  v3 = HermesValue::encodeBigIntValue(b3.get());
  IsSameValueTest(TRUE, v1, v2);
  IsSameValueTest(FALSE, v1, v3);
}

#define AbstractEqualityTest(result, x, y)                          \
  {                                                                 \
    GCScopeMarkerRAII trimmer(gcScope);                             \
    Handle<> xHandle(x);                                            \
    Handle<> yHandle(y);                                            \
    auto res = abstractEqualityTest_RJS(runtime, xHandle, yHandle); \
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus());          \
    EXPECT_##result(*res);                                          \
                                                                    \
    res = abstractEqualityTest_RJS(runtime, yHandle, xHandle);      \
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus());          \
    EXPECT_##result(*res);                                          \
  }

TEST_F(OperationsTest, AbstractEqualityTest) {
  struct : public Locals {
    PinnedValue<StringPrimitive> s1;
    PinnedValue<StringPrimitive> s2;
    PinnedValue<StringPrimitive> s3;
    PinnedValue<StringPrimitive> s4;
    PinnedValue<StringPrimitive> s5;
    PinnedValue<BigIntPrimitive> bigint0;
    PinnedValue<BigIntPrimitive> bigintNoBytes;
    PinnedValue<StringPrimitive> s0;
    PinnedValue<StringPrimitive> sEmpty;
    PinnedValue<BigIntPrimitive> bigint1;
    PinnedValue<StringPrimitive> tmp;
    PinnedValue<BigIntPrimitive> bigint0x00ffff00ffff00;
    PinnedValue<BigIntPrimitive> bigint00ffff00ffff00;
    PinnedValue<StringPrimitive> s281470698520320;
    PinnedValue<StringPrimitive> s281470698520320_0;
    PinnedValue<JSObject> obj1;
    PinnedValue<JSObject> obj2;
    PinnedValue<> v1;
    PinnedValue<> v2;
    PinnedValue<> v3;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  GCScope gcScope{runtime, "OperationsTest.AbstractEqualityTest", 80};

  lv.v1 = HermesValue::encodeNullValue();
  lv.v2 = HermesValue::encodeNullValue();
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeUndefinedValue();
  lv.v2 = HermesValue::encodeUndefinedValue();
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeNullValue();
  lv.v2 = HermesValue::encodeUndefinedValue();
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeBoolValue(true);
  lv.v2 = HermesValue::encodeBoolValue(true);
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeBoolValue(true);
  lv.v2 = HermesValue::encodeBoolValue(false);
  AbstractEqualityTest(FALSE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeUndefinedValue();
  lv.v2 = HermesValue::encodeBoolValue(false);
  AbstractEqualityTest(FALSE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeTrustedNumberValue(123.45);
  lv.v2 = HermesValue::encodeTrustedNumberValue(123.45);
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeTrustedNumberValue(
      std::numeric_limits<double>::quiet_NaN());
  lv.v2 = HermesValue::encodeTrustedNumberValue(
      std::numeric_limits<double>::quiet_NaN());
  AbstractEqualityTest(FALSE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeTrustedNumberValue(
      std::numeric_limits<double>::quiet_NaN());
  lv.v2 = HermesValue::encodeTrustedNumberValue(123.563);
  AbstractEqualityTest(FALSE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeTrustedNumberValue(-0.0);
  lv.v2 = HermesValue::encodeTrustedNumberValue(+0.0);
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeTrustedNumberValue(
      std::numeric_limits<double>::infinity());
  lv.v2 = HermesValue::encodeTrustedNumberValue(
      std::numeric_limits<double>::infinity());
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeTrustedNumberValue(
      std::numeric_limits<double>::infinity());
  lv.v2 = HermesValue::encodeTrustedNumberValue(
      -std::numeric_limits<double>::infinity());
  AbstractEqualityTest(FALSE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeTrustedNumberValue(+0);
  lv.v2 = HermesValue::encodeTrustedNumberValue(+0.0);
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeTrustedNumberValue(-134);
  lv.v2 = HermesValue::encodeTrustedNumberValue(-134.0);
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeTrustedNumberValue(182);
  lv.v2 = HermesValue::encodeTrustedNumberValue(182.54);
  AbstractEqualityTest(FALSE, lv.v1, lv.v2);

  auto nullObj = runtime.makeNullHandle<JSObject>();
  lv.obj1 = JSObject::create(runtime, nullObj);
  lv.obj2 = JSObject::create(runtime, nullObj);

  lv.v1 = HermesValue::encodeObjectValue(lv.obj1.get());
  lv.v2 = HermesValue::encodeObjectValue(lv.obj2.get());
  AbstractEqualityTest(FALSE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeObjectValue(lv.obj1.get());
  lv.v2 = HermesValue::encodeObjectValue(lv.obj1.get());
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  auto s1 =
      createNoThrowForTest(runtime, createUTF16Ref(u"hello world"), lv.s1);
  auto s2 =
      createNoThrowForTest(runtime, createUTF16Ref(u"hello world"), lv.s2);
  auto s3 = createNoThrowForTest(runtime, createUTF16Ref(u"hi earth"), lv.s3);
  lv.v1 = HermesValue::encodeStringValue(s1.get());
  lv.v2 = HermesValue::encodeStringValue(s2.get());
  lv.v3 = HermesValue::encodeStringValue(s3.get());
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);
  AbstractEqualityTest(FALSE, lv.v1, lv.v3);

  auto s4 = createNoThrowForTest(runtime, createUTF16Ref(u"1000"), lv.s4);
  lv.v1 = HermesValue::encodeStringValue(s4.get());
  lv.v2 = HermesValue::encodeTrustedNumberValue(1000);
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  auto s5 = createNoThrowForTest(runtime, createUTF16Ref(u"Infinity"), lv.s5);
  lv.v1 = HermesValue::encodeStringValue(s5.get());
  lv.v2 = HermesValue::encodeTrustedNumberValue(
      std::numeric_limits<double>::infinity());
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeBoolValue(true);
  lv.v2 = HermesValue::encodeTrustedNumberValue(1);
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeBoolValue(false);
  lv.v2 = HermesValue::encodeTrustedNumberValue(0);
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeBoolValue(true);
  lv.v2 = HermesValue::encodeTrustedNumberValue(153);
  AbstractEqualityTest(FALSE, lv.v1, lv.v2);

  // BigInt tests
  // 0n does not equal null, undefined
  auto bigint0 = fromSignedNoThrowForTest(runtime, 0, lv.bigint0);
  lv.v1 = HermesValue::encodeBigIntValue(bigint0.get());
  lv.v2 = HermesValue::encodeNullValue();
  AbstractEqualityTest(FALSE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeBigIntValue(bigint0.get());
  lv.v2 = HermesValue::encodeUndefinedValue();
  AbstractEqualityTest(FALSE, lv.v1, lv.v2);

  // 0n equals bigint created with no bytes
  auto bigintNoBytes = fromBytesNoThrowForTest(runtime, {}, lv.bigintNoBytes);
  lv.v1 = HermesValue::encodeBigIntValue(bigint0.get());
  lv.v2 = HermesValue::encodeBigIntValue(bigintNoBytes.get());
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  // 0n equals false, "", "0", 0.0, +0.0, -0.0
  lv.v1 = HermesValue::encodeBigIntValue(bigint0.get());
  lv.v2 = HermesValue::encodeBoolValue(false);
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  auto s0 = createNoThrowForTest(runtime, createUTF16Ref(u"0"), lv.s0);
  lv.v1 = HermesValue::encodeBigIntValue(bigint0.get());
  lv.v2 = HermesValue::encodeStringValue(s0.get());
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  auto sEmpty = createNoThrowForTest(runtime, createUTF16Ref(u""), lv.sEmpty);
  lv.v1 = HermesValue::encodeBigIntValue(bigint0.get());
  lv.v2 = HermesValue::encodeStringValue(sEmpty.get());
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeBigIntValue(bigint0.get());
  lv.v2 = HermesValue::encodeTrustedNumberValue(0);
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeBigIntValue(bigint0.get());
  lv.v2 = HermesValue::encodeTrustedNumberValue(+0.0);
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeBigIntValue(bigint0.get());
  lv.v2 = HermesValue::encodeTrustedNumberValue(-0.0);
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  // 1n equals true, "1", 1
  auto bigint1 = fromSignedNoThrowForTest(runtime, 1, lv.bigint1);
  s1 = createNoThrowForTest(runtime, createUTF16Ref(u"1"), lv.tmp);

  lv.v1 = HermesValue::encodeBigIntValue(bigint1.get());
  lv.v2 = HermesValue::encodeBoolValue(true);
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeBigIntValue(bigint1.get());
  lv.v2 = HermesValue::encodeStringValue(s1.get());
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeBigIntValue(bigint1.get());
  lv.v2 = HermesValue::encodeTrustedNumberValue(1.0);
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  // Other tests
  auto bigint0x00ffff00ffff00 = fromSignedNoThrowForTest(
      runtime, 0x0000ffff00ffff00ll, lv.bigint0x00ffff00ffff00);
  uint8_t bigint00ffff00ffff00Bytes[] = {
      0x00, 0xff, 0xff, 0x00, 0xff, 0xff, 0x00};
  auto bigint00ffff00ffff00 = fromBytesNoThrowForTest(
      runtime, bigint00ffff00ffff00Bytes, lv.bigint00ffff00ffff00);
  lv.v1 = HermesValue::encodeBigIntValue(bigint0x00ffff00ffff00.get());
  lv.v2 = HermesValue::encodeBigIntValue(bigint00ffff00ffff00.get());
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  auto s281470698520320 = createNoThrowForTest(
      runtime, createUTF16Ref(u"281470698520320"), lv.s281470698520320);
  lv.v1 = HermesValue::encodeBigIntValue(bigint0x00ffff00ffff00.get());
  lv.v2 = HermesValue::encodeStringValue(s281470698520320.get());
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  auto s281470698520320_0 = createNoThrowForTest(
      runtime, createUTF16Ref(u"281470698520320.0"), lv.s281470698520320_0);
  lv.v1 = HermesValue::encodeBigIntValue(bigint0x00ffff00ffff00.get());
  lv.v2 = HermesValue::encodeStringValue(s281470698520320_0.get());
  AbstractEqualityTest(FALSE, lv.v1, lv.v2);

  lv.v1 = HermesValue::encodeBigIntValue(bigint0x00ffff00ffff00.get());
  lv.v2 = HermesValue::encodeTrustedNumberValue(281470698520320.0);
  AbstractEqualityTest(TRUE, lv.v1, lv.v2);

  // TODO: Test Object equality once Runtime::interpretFunction() is written.
}

#define StrictEqualityTest(result, v1, v2)       \
  {                                              \
    EXPECT_##result(strictEqualityTest(v1, v2)); \
    EXPECT_##result(strictEqualityTest(v2, v1)); \
  }

TEST_F(OperationsTest, StrictEquaityTest) {
  struct : public Locals {
    PinnedValue<StringPrimitive> s1;
    PinnedValue<StringPrimitive> s2;
    PinnedValue<StringPrimitive> s3;
    PinnedValue<BigIntPrimitive> bigint0;
    PinnedValue<BigIntPrimitive> bigintNoBytes;
    PinnedValue<StringPrimitive> s0;
    PinnedValue<StringPrimitive> sEmpty;
    PinnedValue<BigIntPrimitive> bigint1;
    PinnedValue<StringPrimitive> tmp;
    PinnedValue<BigIntPrimitive> bigint0x00ffff00ffff00;
    PinnedValue<BigIntPrimitive> bigint00ffff00ffff00;
    PinnedValue<StringPrimitive> s281470698520320;
    PinnedValue<StringPrimitive> s281470698520320_0;
    PinnedValue<JSObject> obj1;
    PinnedValue<JSObject> obj2;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  PinnedHermesValue v1;
  PinnedHermesValue v2;
  PinnedHermesValue v3;

  v1 = HermesValue::encodeNullValue();
  v2 = HermesValue::encodeNullValue();
  StrictEqualityTest(TRUE, v1, v2);

  v1 = HermesValue::encodeUndefinedValue();
  v2 = HermesValue::encodeUndefinedValue();
  StrictEqualityTest(TRUE, v1, v2);

  v1 = HermesValue::encodeNullValue();
  v2 = HermesValue::encodeUndefinedValue();
  StrictEqualityTest(FALSE, v1, v2);

  v1 = HermesValue::encodeBoolValue(true);
  v2 = HermesValue::encodeBoolValue(true);
  StrictEqualityTest(TRUE, v1, v2);

  v1 = HermesValue::encodeBoolValue(true);
  v2 = HermesValue::encodeBoolValue(false);
  StrictEqualityTest(FALSE, v1, v2);

  v1 = HermesValue::encodeUndefinedValue();
  v2 = HermesValue::encodeBoolValue(false);
  StrictEqualityTest(FALSE, v1, v2);

  v1 = HermesValue::encodeTrustedNumberValue(123.45);
  v2 = HermesValue::encodeTrustedNumberValue(123.45);
  StrictEqualityTest(TRUE, v1, v2);

  v1 = HermesValue::encodeTrustedNumberValue(-0.0);
  v2 = HermesValue::encodeTrustedNumberValue(+0.0);
  StrictEqualityTest(TRUE, v1, v2);

  v1 = HermesValue::encodeTrustedNumberValue(
      std::numeric_limits<double>::quiet_NaN());
  v2 = HermesValue::encodeTrustedNumberValue(3);
  StrictEqualityTest(FALSE, v1, v2);

  v1 = HermesValue::encodeTrustedNumberValue(
      std::numeric_limits<double>::quiet_NaN());
  v2 = HermesValue::encodeTrustedNumberValue(
      std::numeric_limits<double>::quiet_NaN());
  StrictEqualityTest(FALSE, v1, v2);

  v1 = HermesValue::encodeTrustedNumberValue(
      std::numeric_limits<double>::infinity());
  v2 = HermesValue::encodeTrustedNumberValue(
      std::numeric_limits<double>::infinity());
  StrictEqualityTest(TRUE, v1, v2);

  v1 = HermesValue::encodeTrustedNumberValue(
      std::numeric_limits<double>::infinity());
  v2 = HermesValue::encodeTrustedNumberValue(
      -std::numeric_limits<double>::infinity());
  StrictEqualityTest(FALSE, v1, v2);

  v1 = HermesValue::encodeTrustedNumberValue(+0);
  v2 = HermesValue::encodeTrustedNumberValue(+0.0);
  StrictEqualityTest(TRUE, v1, v2);

  v1 = HermesValue::encodeTrustedNumberValue(-134);
  v2 = HermesValue::encodeTrustedNumberValue(-134.0);
  StrictEqualityTest(TRUE, v1, v2);

  v1 = HermesValue::encodeTrustedNumberValue(182);
  v2 = HermesValue::encodeTrustedNumberValue(182.54);
  StrictEqualityTest(FALSE, v1, v2);

  auto nullObj = runtime.makeNullHandle<JSObject>();
  lv.obj1 = JSObject::create(runtime, nullObj);
  lv.obj2 = JSObject::create(runtime, nullObj);

  v1 = HermesValue::encodeObjectValue(lv.obj1.get());
  v2 = HermesValue::encodeObjectValue(lv.obj2.get());
  StrictEqualityTest(FALSE, v1, v2);

  v1 = HermesValue::encodeObjectValue(lv.obj1.get());
  v2 = HermesValue::encodeObjectValue(lv.obj1.get());
  StrictEqualityTest(TRUE, v1, v2);

  auto s1 =
      createNoThrowForTest(runtime, createUTF16Ref(u"hello world"), lv.s1);
  auto s2 =
      createNoThrowForTest(runtime, createUTF16Ref(u"hello world"), lv.s2);
  auto s3 = createNoThrowForTest(runtime, createUTF16Ref(u"hi earth"), lv.s3);

  v1 = HermesValue::encodeStringValue(s1.get());
  v2 = HermesValue::encodeStringValue(s2.get());
  v3 = HermesValue::encodeStringValue(s3.get());
  StrictEqualityTest(TRUE, v1, v2);
  StrictEqualityTest(FALSE, v1, v3);

  // BigInt tests
  // 0n does not equal null, undefined
  auto bigint0 = fromSignedNoThrowForTest(runtime, 0, lv.bigint0);
  v1 = HermesValue::encodeBigIntValue(bigint0.get());
  v2 = HermesValue::encodeNullValue();
  StrictEqualityTest(FALSE, v1, v2);

  v1 = HermesValue::encodeBigIntValue(bigint0.get());
  v2 = HermesValue::encodeUndefinedValue();
  StrictEqualityTest(FALSE, v1, v2);

  // 0n equals bigint created with no bytes
  auto bigintNoBytes = fromBytesNoThrowForTest(runtime, {}, lv.bigintNoBytes);
  v1 = HermesValue::encodeBigIntValue(bigint0.get());
  v2 = HermesValue::encodeBigIntValue(bigintNoBytes.get());
  StrictEqualityTest(TRUE, v1, v2);

  // 0n is not strictly equal to false, "", "0", 0.0, +0.0, -0.0
  v1 = HermesValue::encodeBigIntValue(bigint0.get());
  v2 = HermesValue::encodeBoolValue(false);
  StrictEqualityTest(FALSE, v1, v2);

  auto s0 = createNoThrowForTest(runtime, createUTF16Ref(u"0"), lv.s0);
  v1 = HermesValue::encodeBigIntValue(bigint0.get());
  v2 = HermesValue::encodeStringValue(s0.get());
  StrictEqualityTest(FALSE, v1, v2);

  auto sEmpty = createNoThrowForTest(runtime, createUTF16Ref(u""), lv.sEmpty);
  v1 = HermesValue::encodeBigIntValue(bigint0.get());
  v2 = HermesValue::encodeStringValue(sEmpty.get());
  StrictEqualityTest(FALSE, v1, v2);

  v1 = HermesValue::encodeBigIntValue(bigint0.get());
  v2 = HermesValue::encodeTrustedNumberValue(0);
  StrictEqualityTest(FALSE, v1, v2);

  v1 = HermesValue::encodeBigIntValue(bigint0.get());
  v2 = HermesValue::encodeTrustedNumberValue(+0.0);
  StrictEqualityTest(FALSE, v1, v2);

  v1 = HermesValue::encodeBigIntValue(bigint0.get());
  v2 = HermesValue::encodeTrustedNumberValue(-0.0);
  StrictEqualityTest(FALSE, v1, v2);

  // 1n is not strictly equal to true, "1", 1
  auto bigint1 = fromSignedNoThrowForTest(runtime, 1, lv.bigint1);
  s1 = createNoThrowForTest(runtime, createUTF16Ref(u"1"), lv.tmp);

  v1 = HermesValue::encodeBigIntValue(bigint1.get());
  v2 = HermesValue::encodeBoolValue(true);
  StrictEqualityTest(FALSE, v1, v2);

  v1 = HermesValue::encodeBigIntValue(bigint1.get());
  v2 = HermesValue::encodeStringValue(s1.get());
  StrictEqualityTest(FALSE, v1, v2);

  v1 = HermesValue::encodeBigIntValue(bigint1.get());
  v2 = HermesValue::encodeTrustedNumberValue(1.0);
  StrictEqualityTest(FALSE, v1, v2);

  // Other tests
  auto bigint0x00ffff00ffff00 = fromSignedNoThrowForTest(
      runtime, 0x0000ffff00ffff00ll, lv.bigint0x00ffff00ffff00);
  uint8_t bigint00ffff00ffff00Bytes[] = {
      0x00, 0xff, 0xff, 0x00, 0xff, 0xff, 0x00};
  auto bigint00ffff00ffff00 = fromBytesNoThrowForTest(
      runtime, bigint00ffff00ffff00Bytes, lv.bigint00ffff00ffff00);
  v1 = HermesValue::encodeBigIntValue(bigint0x00ffff00ffff00.get());
  v2 = HermesValue::encodeBigIntValue(bigint00ffff00ffff00.get());
  StrictEqualityTest(TRUE, v1, v2);

  auto s281470698520320 = createNoThrowForTest(
      runtime, createUTF16Ref(u"281470698520320"), lv.s281470698520320);
  v1 = HermesValue::encodeBigIntValue(bigint0x00ffff00ffff00.get());
  v2 = HermesValue::encodeStringValue(s281470698520320.get());
  StrictEqualityTest(FALSE, v1, v2);

  auto s281470698520320_0 = createNoThrowForTest(
      runtime, createUTF16Ref(u"281470698520320.0"), lv.s281470698520320_0);
  v1 = HermesValue::encodeBigIntValue(bigint0x00ffff00ffff00.get());
  v2 = HermesValue::encodeStringValue(s281470698520320_0.get());
  StrictEqualityTest(FALSE, v1, v2);

  v1 = HermesValue::encodeBigIntValue(bigint0x00ffff00ffff00.get());
  v2 = HermesValue::encodeTrustedNumberValue(281470698520320.0);
  StrictEqualityTest(FALSE, v1, v2);
}

TEST_F(OperationsTest, IsPrimitiveTest) {
  struct : public Locals {
    PinnedValue<StringPrimitive> s;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  PinnedHermesValue v;

  v = HermesValue::encodeTrustedNumberValue(1);
  EXPECT_TRUE(isPrimitive(v));

  v = HermesValue::encodeNullValue();
  EXPECT_TRUE(isPrimitive(v));

  v = HermesValue::encodeUndefinedValue();
  EXPECT_TRUE(isPrimitive(v));

  v = HermesValue::encodeBoolValue(true);
  EXPECT_TRUE(isPrimitive(v));

  v = HermesValue::encodeTrustedNumberValue(1.21);
  EXPECT_TRUE(isPrimitive(v));

  auto s = createNoThrowForTest(runtime, createUTF16Ref(u"hello world"), lv.s);
  v = HermesValue::encodeStringValue(s.get());
  EXPECT_TRUE(isPrimitive(v));

  auto nullObj = runtime.makeNullHandle<JSObject>();
  auto obj = JSObject::create(runtime, nullObj);
  v = HermesValue::encodeObjectValue(&obj);
  EXPECT_FALSE(isPrimitive(v));
}

TEST_F(OperationsTest, ToBooleanTest) {
  struct : public Locals {
    PinnedValue<StringPrimitive> empty;
    PinnedValue<StringPrimitive> full;
    PinnedValue<BigIntPrimitive> bigint12;
    PinnedValue<BigIntPrimitive> bigint0;
    PinnedValue<BigIntPrimitive> bigintMinus42;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  {
    EXPECT_FALSE(toBoolean(HermesValue::encodeUndefinedValue()));
    EXPECT_FALSE(toBoolean(HermesValue::encodeNullValue()));
  }

  {
    EXPECT_TRUE(toBoolean(HermesValue::encodeBoolValue(true)));
    EXPECT_FALSE(toBoolean(HermesValue::encodeBoolValue(false)));
  }

  {
    auto empty = createNoThrowForTest(runtime, createUTF16Ref(u""), lv.empty);
    EXPECT_FALSE(toBoolean(HermesValue::encodeStringValue(empty.get())));

    auto full = createNoThrowForTest(runtime, createUTF16Ref(u"full"), lv.full);
    EXPECT_TRUE(toBoolean(HermesValue::encodeStringValue(full.get())));
  }

  {
    auto bigint12 = fromSignedNoThrowForTest(runtime, 12, lv.bigint12);
    EXPECT_TRUE(toBoolean(HermesValue::encodeBigIntValue(bigint12.get())));

    auto bigint0 = fromSignedNoThrowForTest(runtime, 0, lv.bigint0);
    EXPECT_FALSE(toBoolean(HermesValue::encodeBigIntValue(bigint0.get())));

    auto bigintMinus42 =
        fromSignedNoThrowForTest(runtime, -42, lv.bigintMinus42);
    EXPECT_TRUE(toBoolean(HermesValue::encodeBigIntValue(bigintMinus42.get())));
  }

  {
    EXPECT_FALSE(toBoolean(HermesValue::encodeTrustedNumberValue(0)));
    EXPECT_FALSE(toBoolean(HermesValue::encodeTrustedNumberValue(-0.0)));
    EXPECT_FALSE(toBoolean(HermesValue::encodeNaNValue()));
    EXPECT_FALSE(toBoolean(HermesValue::encodeTrustedNumberValue(0)));

    EXPECT_TRUE(toBoolean(HermesValue::encodeTrustedNumberValue(123.34)));
    EXPECT_TRUE(toBoolean(HermesValue::encodeTrustedNumberValue(123)));
  }
}

// Use macros for these tests because they're verbose.
#define ToStringTest(result, value)                               \
  {                                                               \
    struct : public Locals {                                      \
      PinnedValue<> scopedValue;                                  \
      PinnedValue<StringPrimitive> strPinned;                     \
    } mlv;                                                        \
    LocalsRAII mlraii(runtime, &mlv);                             \
    mlv.scopedValue = value;                                      \
    auto strRes = toString_RJS(runtime, mlv.scopedValue);         \
    EXPECT_EQ(ExecutionStatus::RETURNED, strRes.getStatus());     \
    mlv.strPinned = std::move(strRes.getValue());                 \
    EXPECT_TRUE(                                                  \
        StringPrimitive::createStringView(runtime, mlv.strPinned) \
            .equals(createUTF16Ref(result)));                     \
  }

#define DoubleToStringTest(result, value) \
  ToStringTest(result, HermesValue::encodeTrustedNumberValue(value));

#define SmallIntToStringTest(result, value) \
  ToStringTest(result, HermesValue::encodeTrustedNumberValue(value));

TEST_F(OperationsTest, ToStringTest) {
  struct : public Locals {
    PinnedValue<StringPrimitive> str1;
    PinnedValue<StringPrimitive> str2;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  GCScope gcScope{runtime, "OperationsTest.ToStringTest", 128};

  // Simple tests
  {
    ToStringTest(u"null", HermesValue::encodeNullValue());
    ToStringTest(u"undefined", HermesValue::encodeUndefinedValue());
    ToStringTest(u"true", HermesValue::encodeBoolValue(true));
    ToStringTest(u"false", HermesValue::encodeBoolValue(false));
  }

  // Strings
  {
    auto str1 = createNoThrowForTest(runtime, createUTF16Ref(u"asdf"), lv.str1);
    ToStringTest(u"asdf", HermesValue::encodeStringValue(str1.get()));
    auto str2 = createNoThrowForTest(runtime, createUTF16Ref(u""), lv.str2);
    ToStringTest(u"", HermesValue::encodeStringValue(str2.get()));
  }

  // Numbers
  {
    DoubleToStringTest(u"NaN", std::numeric_limits<double>::quiet_NaN());

    auto infinity = std::numeric_limits<double>::infinity();
    DoubleToStringTest(u"Infinity", infinity);
    DoubleToStringTest(u"-Infinity", -infinity);

    DoubleToStringTest(u"0", +0.0);
    DoubleToStringTest(u"0", -0.0);
    DoubleToStringTest(u"3", 3);
    DoubleToStringTest(u"-3", -3);
    DoubleToStringTest(u"-10000", -10000);
    DoubleToStringTest(u"1100000", 1.1e6);

    DoubleToStringTest(u"10000", 10000);
    DoubleToStringTest(u"100000", 100000);
    DoubleToStringTest(u"1000000", 1000000);
    DoubleToStringTest(u"10000000", 10000000);
    DoubleToStringTest(u"100000000000000000000", 1e20);
    DoubleToStringTest(u"1e+21", 1e21);

    DoubleToStringTest(u"0.125", 0.125);
    DoubleToStringTest(u"0.5", 0.5);
    DoubleToStringTest(u"3.14", 3.14);
    DoubleToStringTest(u"3.14", 3.14);
    DoubleToStringTest(u"14583.1832", 14583.1832);
    DoubleToStringTest(u"-14583.1832", -14583.1832);
    DoubleToStringTest(u"1.23e+25", 1.23e25);
    DoubleToStringTest(u"1.23e-25", 1.23e-25);
    DoubleToStringTest(u"5e+25", 5e+25);
    DoubleToStringTest(u"-5e+25", -5e+25);

    SmallIntToStringTest(u"0", 0);
    SmallIntToStringTest(u"12384", 12384);
    SmallIntToStringTest(u"-12384", -12384);
  }

  // TODO: Test Object toString once Runtime::interpretFunction() is written.
}

#define ToNumericTest(result, value)                       \
  {                                                        \
    struct : public Locals {                               \
      PinnedValue<> scopedValue;                           \
    } mlv;                                                 \
    LocalsRAII mlraii(runtime, &mlv);                      \
    mlv.scopedValue = value;                               \
    res = toNumeric_RJS(runtime, mlv.scopedValue);         \
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus()); \
    EXPECT_EQ((double)result, res->getDouble());           \
  }

#define InvalidToNumericTest(value)                        \
  {                                                        \
    struct : public Locals {                               \
      PinnedValue<> scopedValue;                           \
    } mlv;                                                 \
    LocalsRAII mlraii(runtime, &mlv);                      \
    mlv.scopedValue = value;                               \
    res = toNumeric_RJS(runtime, mlv.scopedValue);         \
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus()); \
    EXPECT_TRUE(std::isnan(res->getDouble()));             \
  }

#define StringToNumericTest(result, string)                                    \
  {                                                                            \
    auto strPrim =                                                             \
        createNoThrowForTest(runtime, createUTF16Ref(string), lv.strPrimSlot); \
    ToNumericTest(result, HermesValue::encodeStringValue(strPrim.get()));      \
  }

#define InvalidStringToNumericTest(string)                                     \
  {                                                                            \
    auto strPrim =                                                             \
        createNoThrowForTest(runtime, createUTF16Ref(string), lv.strPrimSlot); \
    InvalidToNumericTest(HermesValue::encodeStringValue(strPrim.get()));       \
  }

#define BigIntToNumericTest(result, value)                          \
  {                                                                 \
    struct : public Locals {                                        \
      PinnedValue<> scopedValue;                                    \
      PinnedValue<> scopedResult;                                   \
    } mlv;                                                          \
    LocalsRAII mlraii(runtime, &mlv);                               \
    mlv.scopedValue = value;                                        \
    mlv.scopedResult = result;                                      \
    res = toNumeric_RJS(runtime, mlv.scopedValue);                  \
    ASSERT_EQ(ExecutionStatus::RETURNED, res.getStatus());          \
    ASSERT_TRUE(res->isBigInt());                                   \
    EXPECT_TRUE(mlv.scopedResult->getBigInt() == res->getBigInt()); \
  }

TEST_F(OperationsTest, ToNumericTest) {
  struct : public Locals {
    PinnedValue<BigIntPrimitive> bigintPrim;
    PinnedValue<BigIntPrimitive> bigintPrim2;
    PinnedValue<StringPrimitive> strPrimSlot;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  // Sanity-check a few simple values that should be numbers, not bigints.
  ToNumericTest(+0.0, HermesValue::encodeNullValue());
  InvalidToNumericTest(HermesValue::encodeUndefinedValue());
  ToNumericTest(1, HermesValue::encodeBoolValue(true));
  ToNumericTest(0, HermesValue::encodeBoolValue(false));
  StringToNumericTest(1.0, u"1");

  // ToNumeric(string) should never return a bigint, even for well-formed
  // bigints.
  InvalidStringToNumericTest(u"0n");

  {
    auto bigintPrim = fromSignedNoThrowForTest(runtime, 12, lv.bigintPrim);
    BigIntToNumericTest(
        HermesValue::encodeBigIntValue(bigintPrim.get()),
        HermesValue::encodeBigIntValue(bigintPrim.get()));
  }

  {
    auto bigintPrim = fromSignedNoThrowForTest(runtime, 42, lv.bigintPrim2);
    auto bigintBoxed = toObject(runtime, bigintPrim);
    BigIntToNumericTest(
        HermesValue::encodeBigIntValue(bigintPrim.get()), *bigintBoxed);
  }
}

// Use macros for these tests because they're verbose.
#define ToNumberTest(result, value)                        \
  {                                                        \
    struct : public Locals {                               \
      PinnedValue<> scopedValue;                           \
    } mlv;                                                 \
    LocalsRAII mlraii(runtime, &mlv);                      \
    mlv.scopedValue = value;                               \
    res = toNumber_RJS(runtime, mlv.scopedValue);          \
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus()); \
    EXPECT_EQ((double)result, res->getDouble());           \
  }

#define InvalidToNumberTest(value)                         \
  {                                                        \
    struct : public Locals {                               \
      PinnedValue<> scopedValue;                           \
    } mlv;                                                 \
    LocalsRAII mlraii(runtime, &mlv);                      \
    mlv.scopedValue = value;                               \
    res = toNumber_RJS(runtime, mlv.scopedValue);          \
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus()); \
    EXPECT_TRUE(std::isnan(res->getDouble()));             \
  }

#define StringToNumberTest(result, string)                                     \
  {                                                                            \
    auto strPrim =                                                             \
        createNoThrowForTest(runtime, createUTF16Ref(string), lv.strPrimSlot); \
    ToNumberTest(result, HermesValue::encodeStringValue(strPrim.get()));       \
  }

#define InvalidStringToNumberTest(string)                                      \
  {                                                                            \
    auto strPrim =                                                             \
        createNoThrowForTest(runtime, createUTF16Ref(string), lv.strPrimSlot); \
    InvalidToNumberTest(HermesValue::encodeStringValue(strPrim.get()));        \
  }

using OperationsLargeHeapTest = LargeHeapRuntimeTestFixture;

TEST_F(OperationsLargeHeapTest, ToNumberTest) {
  struct : public Locals {
    PinnedValue<StringPrimitive> str;
    PinnedValue<BigIntPrimitive> bigintPrim;
    PinnedValue<StringPrimitive> strPrimSlot;
    PinnedValue<> scopedVal;
    PinnedValue<> numVal;
    PinnedValue<StringPrimitive> strResPinned;
    PinnedValue<> hBigInt;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  GCScope gcScope{runtime, "OperationsTest.ToNumberTest", 200};

  // Simple tests
  {
    ToNumberTest(+0.0, HermesValue::encodeNullValue());
    InvalidToNumberTest(HermesValue::encodeUndefinedValue());
    ToNumberTest(1, HermesValue::encodeBoolValue(true));
    ToNumberTest(0, HermesValue::encodeBoolValue(false));
  }

  // Strings
  {
    // Infinity
    StringToNumberTest(std::numeric_limits<double>::infinity(), u"Infinity");
    StringToNumberTest(std::numeric_limits<double>::infinity(), u"+Infinity");
    StringToNumberTest(-std::numeric_limits<double>::infinity(), u"-Infinity");
    StringToNumberTest(std::numeric_limits<double>::infinity(), u" Infinity\t");
    StringToNumberTest(std::numeric_limits<double>::infinity(), u" +Infinity ");
    StringToNumberTest(-std::numeric_limits<double>::infinity(), u" -Infinity");

    // Integers
    StringToNumberTest(0, u"");
    StringToNumberTest(0, u"\t   ");
    StringToNumberTest(0, u"   0");
    StringToNumberTest(0, u"0   ");
    StringToNumberTest(3, u"   \t  3 \t \n \u2028 \uFEFF ");
    StringToNumberTest(3, u"3");
    StringToNumberTest(3, u"00000000000000000000000000000003");
    StringToNumberTest(-3, u"-3");
    StringToNumberTest(10000, u"10000");
    StringToNumberTest(-10000, u"-10000");
    StringToNumberTest(2147483647, u"2147483647");
    StringToNumberTest(-2147483647, u"-2147483647");

    // Signed zero
    StringToNumberTest(0, u"+0.0");
    EXPECT_FALSE(std::signbit(res->getDouble()));
    StringToNumberTest(0, u"-0.0");
    EXPECT_TRUE(std::signbit(res->getDouble()));

    // Non-integrals
    StringToNumberTest(3.14, u"3.14");
    StringToNumberTest(14583.1832, u"14583.1832");
    StringToNumberTest(-14583.1832, u"-14583.1832");
    StringToNumberTest(1.23e25, u"1.23e+25");
    StringToNumberTest(1.23e-25, u"1.23e-25");
    StringToNumberTest(13e8, u"13e8");
    StringToNumberTest(.11248, u".11248");
    StringToNumberTest(6853294837411.219, u"6853294837411.2184921374291384");
    StringToNumberTest(
        1.0948273409128347e+43,
        u"10948273409128347210948271309487321094712389");

    // Scientific notation
    StringToNumberTest(1.23e+25, u"1.23e+25");
    StringToNumberTest(1.23e+25, u"1.23e25");
    StringToNumberTest(1.23e+25, u"1.23E+25");
    StringToNumberTest(1.23e+25, u"1.23E25");
    StringToNumberTest(1.23e+25, u"+1.23E25");
    StringToNumberTest(1.23e-25, u"+1.23E-25");
    StringToNumberTest(1.23e-25, u"+1.23e-25");
    StringToNumberTest(-1.23e-25, u"-1.23E-25");
    StringToNumberTest(4.3214123123e+29, u"43214.123123e25");
    StringToNumberTest(
        std::numeric_limits<double>::infinity(), u"6143812.482134891732e12904");

    // Hex
    StringToNumberTest(0x234aL, u"0x234a");
    StringToNumberTest(0x234aL, u"0X234A");
    StringToNumberTest(
        6.582018229284824e+63,
        u"0xfffffffffffffffffffffffffffffffffffffffffffffffffffff");
    StringToNumberTest(0xdeadbeefL, u"0xdeAdBeeF");

    // Special hex
    StringToNumberTest(1152921504606847200, u"0x1000000000000081");
    StringToNumberTest(1152921504606847200, u"0x1000000000000084");
    StringToNumberTest(1152921504606847000, u"0x1000000000000079");

    // Octal
    StringToNumberTest(0, u"0o0");
    StringToNumberTest(9, u"0o11");

    // Binary
    StringToNumberTest(0, u"0b0");
    StringToNumberTest(3, u"0b11");
    StringToNumberTest(1024, u"0b10000000000");
  }

  // Big hex number
  {
    auto str = createNoThrowForTest(
        runtime, createUTF16Ref(u"0xFFFFFFFFFFFFFFFF"), lv.str);
    lv.scopedVal = HermesValue::encodeStringValue(str.get());
    res = toNumber_RJS(runtime, lv.scopedVal);
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus());
    lv.numVal = res.getValue();
    auto strRes = toString_RJS(runtime, lv.numVal);
    EXPECT_EQ(ExecutionStatus::RETURNED, strRes.getStatus());
    lv.strResPinned = std::move(*strRes);
    EXPECT_TRUE(
        StringPrimitive::createStringView(runtime, lv.strResPinned)
            .equals(createUTF16Ref(u"18446744073709552000")));
  }

  // Invalid strings
  {
    InvalidStringToNumberTest(u"NaN");
    InvalidStringToNumberTest(u"  NaN  ");
    InvalidStringToNumberTest(u"0x");
    InvalidStringToNumberTest(u"  0x");
    InvalidStringToNumberTest(u"0x123E-4");
    InvalidStringToNumberTest(u"-0x234a");
    InvalidStringToNumberTest(u"-0xdeAdBeeF");
    InvalidStringToNumberTest(u"0xDEADBEEG");
    InvalidStringToNumberTest(u"0xdeadbeeg");
    InvalidStringToNumberTest(u"aawpfeioj");
    InvalidStringToNumberTest(u"123asdf");
    InvalidStringToNumberTest(u"123.123.123.123");
    InvalidStringToNumberTest(u"4 6 7 ");
    InvalidStringToNumberTest(u" 1 a ");
    InvalidStringToNumberTest(u"!(*#&!");
    InvalidStringToNumberTest(u"1֍  ");
  }

  // BigInt
  {
    auto bigintPrim = fromSignedNoThrowForTest(runtime, 0, lv.bigintPrim);
    lv.hBigInt = HermesValue::encodeBigIntValue(bigintPrim.get());
    res = toNumber_RJS(runtime, lv.hBigInt);
    ASSERT_EQ(ExecutionStatus::EXCEPTION, res.getStatus());
    HermesValue thrownValue = runtime.getThrownValue();
    ASSERT_TRUE(thrownValue.isObject());
    auto *thrownJSObject = dyn_vmcast<JSObject>(thrownValue.getObject(runtime));
    ASSERT_TRUE(thrownJSObject);
    auto thrownProto =
        JSObject::getPrototypeOf(createPseudoHandle(thrownJSObject), runtime);
    ASSERT_NE(ExecutionStatus::EXCEPTION, thrownProto.getStatus());
    EXPECT_EQ(*runtime.TypeErrorPrototype, thrownProto->get());
    runtime.clearThrownValue();
  }

  // TODO: Test Object toNumber once Runtime::interpretFunction() is written.
}

#define ToIntegerTest(result, value)                       \
  {                                                        \
    struct : public Locals {                               \
      PinnedValue<> scopedValue;                           \
    } mlv;                                                 \
    LocalsRAII mlraii(runtime, &mlv);                      \
    mlv.scopedValue = value;                               \
    res = toIntegerOrInfinity(runtime, mlv.scopedValue);   \
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus()); \
    EXPECT_EQ(result, res->getNumber());                   \
  }

TEST_F(OperationsTest, ToIntegerTest) {
  ToIntegerTest(0, HermesValue::encodeNullValue());
  ToIntegerTest(0, HermesValue::encodeUndefinedValue());

  ToIntegerTest(2, HermesValue::encodeTrustedNumberValue(2.0));
  ToIntegerTest(2, HermesValue::encodeTrustedNumberValue(2.4));
  ToIntegerTest(-2, HermesValue::encodeTrustedNumberValue(-2.4));
  ToIntegerTest(
      0,
      HermesValue::encodeTrustedNumberValue(
          std::numeric_limits<double>::quiet_NaN()));
  ToIntegerTest(0, HermesValue::encodeTrustedNumberValue(-0.0));
  ToIntegerTest(0, HermesValue::encodeTrustedNumberValue(+0.0));

  ToIntegerTest(
      std::numeric_limits<double>::infinity(),
      HermesValue::encodeTrustedNumberValue(
          std::numeric_limits<double>::infinity()));
  ToIntegerTest(
      -std::numeric_limits<double>::infinity(),
      HermesValue::encodeTrustedNumberValue(
          -std::numeric_limits<double>::infinity()));
}

#define ToInt32Test(result, value)                         \
  {                                                        \
    struct : public Locals {                               \
      PinnedValue<> scopedValue;                           \
    } mlv;                                                 \
    LocalsRAII mlraii(runtime, &mlv);                      \
    mlv.scopedValue = value;                               \
    res = toInt32_RJS(runtime, mlv.scopedValue);           \
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus()); \
    EXPECT_EQ(result, res->getNumber());                   \
  }

TEST_F(OperationsTest, ToInt32Test) {
  ToInt32Test(0, HermesValue::encodeNullValue());
  ToInt32Test(0, HermesValue::encodeUndefinedValue());

  ToInt32Test(
      0,
      HermesValue::encodeTrustedNumberValue(
          std::numeric_limits<double>::infinity()));
  ToInt32Test(
      0,
      HermesValue::encodeTrustedNumberValue(
          -std::numeric_limits<double>::infinity()));

  ToInt32Test(2, HermesValue::encodeTrustedNumberValue(2.0));
  ToInt32Test(2, HermesValue::encodeTrustedNumberValue(2.4));
  ToInt32Test(-2, HermesValue::encodeTrustedNumberValue(-2.4));
  ToInt32Test(
      0,
      HermesValue::encodeTrustedNumberValue(
          std::numeric_limits<double>::quiet_NaN()));
  ToInt32Test(0, HermesValue::encodeTrustedNumberValue(-0.0));
  ToInt32Test(0, HermesValue::encodeTrustedNumberValue(+0.0));

  ToInt32Test(1, HermesValue::encodeTrustedNumberValue(std::pow(2, 33) + 1.0));
  ToInt32Test(-1, HermesValue::encodeTrustedNumberValue(std::pow(2, 33) - 1.0));
  ToInt32Test(
      1238, HermesValue::encodeTrustedNumberValue(std::pow(2, 33) + 1238.0));
  ToInt32Test(
      -1238, HermesValue::encodeTrustedNumberValue(std::pow(2, 33) - 1238.0));
}

#define ToUInt32Test(result, value)                        \
  {                                                        \
    struct : public Locals {                               \
      PinnedValue<> scopedValue;                           \
    } mlv;                                                 \
    LocalsRAII mlraii(runtime, &mlv);                      \
    mlv.scopedValue = value;                               \
    res = toUInt32_RJS(runtime, mlv.scopedValue);          \
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus()); \
    EXPECT_EQ(result, res->getNumber());                   \
  }

TEST_F(OperationsTest, ToUInt32Test) {
  ToUInt32Test(0, HermesValue::encodeNullValue());
  ToUInt32Test(0, HermesValue::encodeUndefinedValue());

  ToUInt32Test(
      0,
      HermesValue::encodeTrustedNumberValue(
          std::numeric_limits<double>::infinity()));
  ToUInt32Test(
      0,
      HermesValue::encodeTrustedNumberValue(
          -std::numeric_limits<double>::infinity()));

  ToUInt32Test(2, HermesValue::encodeTrustedNumberValue(2.0));
  ToUInt32Test(2, HermesValue::encodeTrustedNumberValue(2.4));
  ToUInt32Test(
      std::pow(2, 32) - 2, HermesValue::encodeTrustedNumberValue(-2.4));
  ToUInt32Test(
      0,
      HermesValue::encodeTrustedNumberValue(
          std::numeric_limits<double>::quiet_NaN()));
  ToUInt32Test(0, HermesValue::encodeTrustedNumberValue(-0.0));
  ToUInt32Test(0, HermesValue::encodeTrustedNumberValue(+0.0));

  ToUInt32Test(1, HermesValue::encodeTrustedNumberValue(std::pow(2, 33) + 1.0));
  ToUInt32Test(
      std::pow(2, 32) - 1,
      HermesValue::encodeTrustedNumberValue(std::pow(2, 33) - 1.0));
  ToUInt32Test(
      1238, HermesValue::encodeTrustedNumberValue(std::pow(2, 33) + 1238.0));
  ToUInt32Test(
      std::pow(2, 32) - 1238,
      HermesValue::encodeTrustedNumberValue(std::pow(2, 33) - 1238.0));
}

#define ToUInt16Test(result, value)                        \
  {                                                        \
    struct : public Locals {                               \
      PinnedValue<> scopedValue;                           \
    } mlv;                                                 \
    LocalsRAII mlraii(runtime, &mlv);                      \
    mlv.scopedValue = value;                               \
    res = toUInt16(runtime, mlv.scopedValue);              \
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus()); \
    EXPECT_EQ(result, res->getNumber());                   \
  }

TEST_F(OperationsTest, ToUInt16Test) {
  ToUInt16Test(0, HermesValue::encodeNullValue());
  ToUInt16Test(0, HermesValue::encodeUndefinedValue());

  ToUInt16Test(
      0,
      HermesValue::encodeTrustedNumberValue(
          std::numeric_limits<double>::infinity()));
  ToUInt16Test(
      0,
      HermesValue::encodeTrustedNumberValue(
          -std::numeric_limits<double>::infinity()));

  ToUInt16Test(2, HermesValue::encodeTrustedNumberValue(2.0));
  ToUInt16Test(2, HermesValue::encodeTrustedNumberValue(2.4));
  ToUInt16Test(
      std::pow(2, 16) - 2, HermesValue::encodeTrustedNumberValue(-2.4));
  ToUInt16Test(
      0,
      HermesValue::encodeTrustedNumberValue(
          std::numeric_limits<double>::quiet_NaN()));
  ToUInt16Test(0, HermesValue::encodeTrustedNumberValue(-0.0));
  ToUInt16Test(0, HermesValue::encodeTrustedNumberValue(+0.0));

  ToUInt16Test(1, HermesValue::encodeTrustedNumberValue(std::pow(2, 18) + 1.0));
  ToUInt16Test(
      std::pow(2, 16) - 1,
      HermesValue::encodeTrustedNumberValue(std::pow(2, 18) - 1.0));
  ToUInt16Test(
      1238, HermesValue::encodeTrustedNumberValue(std::pow(2, 33) + 1238.0));
  ToUInt16Test(
      std::pow(2, 16) - 1238,
      HermesValue::encodeTrustedNumberValue(std::pow(2, 18) - 1238.0));
}

TEST_F(OperationsTest, ToObjectTest) {
  struct : public Locals {
    PinnedValue<StringPrimitive> str;
    PinnedValue<BigIntPrimitive> bigint;
    PinnedValue<> scopedVal;
    PinnedValue<StringPrimitive> objStr;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  {
    lv.scopedVal = HermesValue::encodeNullValue();
    EXPECT_EQ(
        ExecutionStatus::EXCEPTION,
        toObject(runtime, lv.scopedVal).getStatus());
    runtime.clearThrownValue();
  }

  {
    lv.scopedVal = HermesValue::encodeUndefinedValue();
    EXPECT_EQ(
        ExecutionStatus::EXCEPTION,
        toObject(runtime, lv.scopedVal).getStatus());
    runtime.clearThrownValue();
  }

  {
    lv.scopedVal = HermesValue::encodeBoolValue(true);
    auto res = toObject(runtime, lv.scopedVal);
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus());
    EXPECT_TRUE(res->isObject());
    auto obj = vmcast<JSBoolean>(static_cast<GCCell *>(res->getObject()));
    EXPECT_TRUE(obj->getPrimitiveBoolean());
  }

  {
    auto m = 1529.184;
    lv.scopedVal = HermesValue::encodeTrustedNumberValue(m);
    auto res = toObject(runtime, lv.scopedVal);
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus());
    EXPECT_TRUE(res->isObject());
    auto obj = vmcast<JSNumber>(static_cast<GCCell *>(res->getObject()));
    EXPECT_EQ(m, obj->getPrimitiveNumber());
  }

  {
    auto strRef = createUTF16Ref(u"hello");
    auto str = createNoThrowForTest(runtime, strRef, lv.str);
    lv.scopedVal = HermesValue::encodeStringValue(str.get());
    auto res = toObject(runtime, lv.scopedVal);
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus());
    EXPECT_TRUE(res->isObject());
    auto obj = vmcast<JSString>(static_cast<GCCell *>(res->getObject()));
    lv.objStr = JSString::getPrimitiveString(obj, runtime);
    EXPECT_TRUE(
        StringPrimitive::createStringView(runtime, lv.objStr).equals(strRef));
  }

  {
    auto bigint = fromSignedNoThrowForTest(runtime, 0x7ffffffe, lv.bigint);
    lv.scopedVal = HermesValue::encodeBigIntValue(bigint.get());
    auto res = toObject(runtime, lv.scopedVal);
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus());
    EXPECT_TRUE(res->isObject());
    auto obj = vmcast<JSBigInt>(static_cast<GCCell *>(res->getObject()));
    // Although not required by the spec, Hermes' implementation is expected to
    // not create a new BigIntPrimitive when boxing. Thus, make sure the
    // addresses match.
    EXPECT_TRUE(bigint.get() == JSBigInt::getPrimitiveBigInt(obj, runtime));
  }
}

#define TypeOfTest(result, hv)                                          \
  do {                                                                  \
    struct : public Locals {                                            \
      PinnedValue<> handleValue;                                        \
      PinnedValue<StringPrimitive> str;                                 \
    } mlv;                                                              \
    LocalsRAII mlraii(runtime, &mlv);                                   \
    mlv.handleValue = hv;                                               \
    auto res = typeOf(runtime, mlv.handleValue);                        \
    ASSERT_TRUE(res.isString());                                        \
    mlv.str = res.getString();                                          \
    auto strView = StringPrimitive::createStringView(runtime, mlv.str); \
    EXPECT_TRUE(strView.equals(createUTF16Ref(result)));                \
  } while (0)

TEST_F(OperationsTest, typeOfTest) {
  struct : public Locals {
    PinnedValue<StringPrimitive> tmp;
    PinnedValue<BigIntPrimitive> tmp2;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  TypeOfTest(u"undefined", HermesValue::encodeUndefinedValue());

  TypeOfTest(u"object", HermesValue::encodeNullValue());

  TypeOfTest(u"boolean", HermesValue::encodeBoolValue(false));
  TypeOfTest(u"boolean", HermesValue::encodeBoolValue(true));

  TypeOfTest(u"number", HermesValue::encodeTrustedNumberValue(+0.0));
  TypeOfTest(u"number", HermesValue::encodeNaNValue());

  TypeOfTest(
      u"string",
      createNoThrowForTest(runtime, createUTF16Ref(u""), lv.tmp)
          .getHermesValue());

  TypeOfTest(
      u"bigint",
      fromSignedNoThrowForTest(runtime, 0, lv.tmp2).getHermesValue());

  TypeOfTest(
      u"symbol", HermesValue::encodeSymbolValue(SymbolID::unsafeCreate(1)));
}

#define GetPrimitivePrototypeTest(result, hv)                   \
  do {                                                          \
    struct : public Locals {                                    \
      PinnedValue<> handleValue;                                \
    } mlv;                                                      \
    LocalsRAII mlraii(runtime, &mlv);                           \
    mlv.handleValue = hv;                                       \
    auto res = getPrimitivePrototype(runtime, mlv.handleValue); \
    ASSERT_EQ(ExecutionStatus::RETURNED, res.getStatus());      \
    EXPECT_TRUE(Handle<JSObject>::vmcast(&result) == *res);     \
  } while (0)

#define GetPrimitivePrototypeThrowsTest(hv)                     \
  do {                                                          \
    struct : public Locals {                                    \
      PinnedValue<> handleValue;                                \
    } mlv;                                                      \
    LocalsRAII mlraii(runtime, &mlv);                           \
    mlv.handleValue = hv;                                       \
    auto res = getPrimitivePrototype(runtime, mlv.handleValue); \
    EXPECT_EQ(ExecutionStatus::EXCEPTION, res.getStatus());     \
    runtime.clearThrownValue();                                 \
  } while (0)

TEST_F(OperationsTest, getPrimitivePrototypeTest) {
  struct : public Locals {
    PinnedValue<StringPrimitive> tmp;
    PinnedValue<BigIntPrimitive> tmp2;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  GetPrimitivePrototypeThrowsTest(HermesValue::encodeUndefinedValue());

  GetPrimitivePrototypeThrowsTest(HermesValue::encodeNullValue());

  GetPrimitivePrototypeTest(
      runtime.booleanPrototype, HermesValue::encodeBoolValue(false));
  GetPrimitivePrototypeTest(
      runtime.booleanPrototype, HermesValue::encodeBoolValue(true));

  GetPrimitivePrototypeTest(
      runtime.numberPrototype, HermesValue::encodeTrustedNumberValue(+0.0));
  GetPrimitivePrototypeTest(
      runtime.numberPrototype, HermesValue::encodeNaNValue());

  GetPrimitivePrototypeTest(
      runtime.stringPrototype,
      createNoThrowForTest(runtime, createUTF16Ref(u""), lv.tmp)
          .getHermesValue());

  GetPrimitivePrototypeTest(
      runtime.bigintPrototype,
      fromSignedNoThrowForTest(runtime, 0, lv.tmp2).getHermesValue());

  GetPrimitivePrototypeTest(
      runtime.symbolPrototype,
      HermesValue::encodeSymbolValue(SymbolID::unsafeCreate(1)));
}

#define NumberAdditionTest(result, x, y)                   \
  {                                                        \
    struct : public Locals {                               \
      PinnedValue<> xv;                                    \
      PinnedValue<> yv;                                    \
    } mlv;                                                 \
    LocalsRAII mlraii(runtime, &mlv);                      \
    mlv.xv = x;                                            \
    mlv.yv = y;                                            \
    res = addOp_RJS(runtime, mlv.xv, mlv.yv);              \
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus()); \
    EXPECT_TRUE(res.getValue().isNumber());                \
    EXPECT_EQ(result, res->getNumber());                   \
  }

#define InvalidNumberAdditionTest(x, y)                    \
  {                                                        \
    struct : public Locals {                               \
      PinnedValue<> xv;                                    \
      PinnedValue<> yv;                                    \
    } mlv;                                                 \
    LocalsRAII mlraii(runtime, &mlv);                      \
    mlv.xv = x;                                            \
    mlv.yv = y;                                            \
    res = addOp_RJS(runtime, mlv.xv, mlv.yv);              \
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus()); \
    EXPECT_TRUE(res.getValue().isNumber());                \
    EXPECT_TRUE(std::isnan(res->getNumber()));             \
  }

#define DoubleAdditionTest(result, x, y)           \
  {                                                \
    NumberAdditionTest(                            \
        result,                                    \
        HermesValue::encodeTrustedNumberValue(x),  \
        HermesValue::encodeTrustedNumberValue(y)); \
  }

TEST_F(OperationsTest, AdditionTest) {
  struct : public Locals {
    PinnedValue<StringPrimitive> a;
    PinnedValue<StringPrimitive> b;
    PinnedValue<StringPrimitive> a2;
    PinnedValue<> bNum;
    PinnedValue<StringPrimitive> resStr;
  } lv;
  LocalsRAII lraii_lv(runtime, &lv);
  GCScope gcScope{runtime, "OperationsTest.AdditionTest", 128};

  {
    auto a = createNoThrowForTest(runtime, createUTF16Ref(u"abc"), lv.a);
    auto b = createNoThrowForTest(runtime, createUTF16Ref(u"def"), lv.b);
    res = addOp_RJS(runtime, a, b);
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus());
    EXPECT_TRUE(res.getValue().isString());
    lv.resStr.castAndSetHermesValue<StringPrimitive>(res.getValue());
    EXPECT_TRUE(
        StringPrimitive::createStringView(runtime, lv.resStr)
            .equals(createUTF16Ref(u"abcdef")));
  }

  {
    auto a = createNoThrowForTest(runtime, createUTF16Ref(u"number: "), lv.a2);
    lv.bNum = HermesValue::encodeTrustedNumberValue(1.4);
    res = addOp_RJS(runtime, a, lv.bNum);
    EXPECT_EQ(ExecutionStatus::RETURNED, res.getStatus());
    EXPECT_TRUE(res.getValue().isString());
    lv.resStr.castAndSetHermesValue<StringPrimitive>(res.getValue());
    EXPECT_TRUE(
        StringPrimitive::createStringView(runtime, lv.resStr)
            .equals(createUTF16Ref(u"number: 1.4")));
  }

  {
    InvalidNumberAdditionTest(
        HermesValue::encodeTrustedNumberValue(
            std::numeric_limits<double>::quiet_NaN()),
        HermesValue::encodeTrustedNumberValue(1));
    InvalidNumberAdditionTest(
        HermesValue::encodeTrustedNumberValue(1),
        HermesValue::encodeTrustedNumberValue(
            std::numeric_limits<double>::quiet_NaN()));
  }

  {
    auto inf = std::numeric_limits<double>::infinity();
    InvalidNumberAdditionTest(
        HermesValue::encodeTrustedNumberValue(inf),
        HermesValue::encodeTrustedNumberValue(-inf));
    DoubleAdditionTest(inf, inf, inf);
    DoubleAdditionTest(-inf, -inf, -inf);
    DoubleAdditionTest(inf, inf, 4);
    DoubleAdditionTest(-inf, -inf, 4);
  }

  {
    DoubleAdditionTest(5, 0, 5);
    DoubleAdditionTest(5, -0, 5);

    DoubleAdditionTest(+0.0, -5, 5);
    EXPECT_FALSE(std::signbit(res->getNumber()));

    DoubleAdditionTest(-0.0, -0.0, -0.0);
    EXPECT_TRUE(std::signbit(res->getNumber()));

    DoubleAdditionTest(-0.0, +0.0, +0.0);
    EXPECT_FALSE(std::signbit(res->getNumber()));

    DoubleAdditionTest(-0.0, -0.0, 0.0);
    EXPECT_FALSE(std::signbit(res->getNumber()));
  }

  {
    NumberAdditionTest(
        4.0,
        HermesValue::encodeTrustedNumberValue(3.0),
        HermesValue::encodeBoolValue(true));
    NumberAdditionTest(
        3.0,
        HermesValue::encodeTrustedNumberValue(3.0),
        HermesValue::encodeBoolValue(false));
  }

  {
    InvalidNumberAdditionTest(
        HermesValue::encodeTrustedNumberValue(3.0),
        HermesValue::encodeUndefinedValue());
    NumberAdditionTest(
        3.0,
        HermesValue::encodeTrustedNumberValue(3.0),
        HermesValue::encodeNullValue());
  }

  {
    DoubleAdditionTest(2, 1, 1);
    DoubleAdditionTest(185, 189, -4);
    DoubleAdditionTest(3528.142, 3527, 1.142);
  }
}

TEST_F(OperationsTest, NumberToBigIntTest) {
  // Non-integral inputs to numberToBigInt should result in an exception.
  {
    auto res = numberToBigInt(runtime, 0.5);
    ASSERT_EQ(ExecutionStatus::EXCEPTION, res.getStatus());
    HermesValue thrownValue = runtime.getThrownValue();
    ASSERT_TRUE(thrownValue.isObject());
    auto *thrownJSObject = dyn_vmcast<JSObject>(thrownValue.getObject(runtime));
    ASSERT_TRUE(thrownJSObject);
    auto thrownProto =
        JSObject::getPrototypeOf(createPseudoHandle(thrownJSObject), runtime);
    ASSERT_NE(ExecutionStatus::EXCEPTION, thrownProto.getStatus());
    EXPECT_EQ(*runtime.RangeErrorPrototype, thrownProto->get());
    runtime.clearThrownValue();
  }
}
} // namespace

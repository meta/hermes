/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VMRuntimeTestHelpers.h"
#include "hermes/BCGen/HBC/Bytecode.h"
#include "hermes/VM/DummyObject.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StringPrimitive.h"

#include "gtest/gtest.h"

using namespace hermes::vm;
using namespace hermes::vm::testhelpers;

namespace {

using HandleTest = RuntimeTestFixture;
using HandleDeathTest = RuntimeTestFixture;

TEST_F(HandleTest, AddAndRemoveTest) {
  auto str1 = createNoThrowForTest(runtime, createUTF16Ref(u"str1"));
  auto str2 = createNoThrowForTest(runtime, createUTF16Ref(u"str2"));
  auto str3 = createNoThrowForTest(runtime, createUTF16Ref(u"str3"));

  {
    auto h1 = runtime.makeHandle<StringPrimitive>(str1.get());
    {
      GCScope scope(runtime);
      auto h2 = runtime.makeHandle<StringPrimitive>(str2.get());
      auto h3 = runtime.makeHandle<StringPrimitive>(str3.get());

      ASSERT_EQ(str1, h1);
      ASSERT_EQ(str2, h2);
      ASSERT_EQ(str3, h3);
    }
    ASSERT_EQ(str1, h1);

    {
      GCScope scope(runtime);
      // Check operator=.
      Handle<StringPrimitive> h3 = h1;
      MutableHandle<StringPrimitive> h4(runtime, h1.get());
      Handle<StringPrimitive> h5 = h4;

      ASSERT_EQ(str1, h3);
      ASSERT_EQ(str1, h4);
      ASSERT_EQ(str1, h5);
      h4 = h1.get();

      ASSERT_EQ(str1, h4);
    }
    {
      GCScope scope(runtime);
      // Check copy constructor.
      Handle<StringPrimitive> h3(h1);
      Handle<StringPrimitive> h4(h1);

      ASSERT_EQ(str1, h3);
      ASSERT_EQ(str1, h4);
    }

    {
      GCScope scope(runtime);
      // Check mov constructor.
      auto h5 = runtime.makeHandle<StringPrimitive>(str2.get());
      ASSERT_EQ(str2, h5);
    }
  }
}

TEST_F(HandleTest, ValueAddAndRemoveTest) {
  auto str1 = runtime.makeHandle(HermesValue::encodeTrustedNumberValue(1));
  auto str2 = runtime.makeHandle(HermesValue::encodeTrustedNumberValue(2));
  auto str3 = runtime.makeHandle(HermesValue::encodeTrustedNumberValue(3));

  {
    MutableHandle<> h1(runtime, str1.get());
    {
      MutableHandle<> h2(runtime, str2.get());
      auto h3 = runtime.makeHandle(str3.get());

      ASSERT_EQ(str1, h1);
      ASSERT_EQ(str2, h2);
      ASSERT_EQ(str3, h3);

      h2.set(h3.get());
      ASSERT_EQ(str3, h2);
    }
    // h2 is out of scope, so it should be gone.

    {
      // Check operator=.
      Handle<> h3 = h1;
      MutableHandle<> h4(runtime, h1.get());
      Handle<> h5 = h4;

      ASSERT_EQ(str1, h3);
      ASSERT_EQ(str1, h4);
      ASSERT_EQ(str1, h5);
      h4 = h1.get();

      ASSERT_EQ(str1, h4);
    }
    {
      // Check copy constructor.
      Handle<> h3(h1);
      Handle<> h4(h1);

      ASSERT_EQ(str1, h3);
      ASSERT_EQ(str1, h4);
    }

    {
      // Check mov constructor.
      MutableHandle<> h5(runtime, h1.get());
      MutableHandle<> h6(std::move(h5));
      ASSERT_EQ(str1, h6);
    }
  }
}

TEST_F(HandleTest, MarkTest) {
  GCScope gcScope{runtime};
  auto v1 = runtime.makeHandle(HermesValue::encodeTrustedNumberValue(1));
  (void)v1;
  auto v2 = runtime.makeHandle(HermesValue::encodeTrustedNumberValue(2));
  (void)v2;
  ASSERT_EQ(2u, gcScope.getHandleCountDbg());

  auto marker = gcScope.createMarker();

  auto v3 = runtime.makeHandle(HermesValue::encodeTrustedNumberValue(3));
  (void)v3;
  ASSERT_EQ(3u, gcScope.getHandleCountDbg());

  gcScope.flushToMarker(marker);
  ASSERT_EQ(2u, gcScope.getHandleCountDbg());

  auto v4 = runtime.makeHandle(HermesValue::encodeTrustedNumberValue(4));
  (void)v4;
  ASSERT_EQ(3u, gcScope.getHandleCountDbg());

  gcScope.flushToMarker(marker);
  ASSERT_EQ(2u, gcScope.getHandleCountDbg());
}

/// Make sure that related Handle-s can be assigned.
TEST_F(HandleTest, ScopedPointerConstructorTest) {
  auto function = runtime.makeHandle<JSFunction>(
      JSFunction::create(runtime, domain, runtime.makeNullHandle<JSObject>()));
  Handle<JSObject> obj = function;
  ASSERT_EQ(function.get(), obj.get());
}

TEST_F(HandleTest, PinnedValueHV32Test) {
  struct : Locals {
    PinnedValue<DummyObject> obj;
    PinnedValue<SmallHermesValue> pshv;
    PinnedValue<StringPrimitive> str;
    PinnedValue<SymbolID> sym;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.obj = testhelpers::DummyObject::create(runtime.getHeap(), runtime);
  lv.pshv = SmallHermesValue::encodeObjectValue(*lv.obj, runtime);
  runtime.collect("test");
  {
    auto shv = lv.pshv.getSmallHermesValue();
    ASSERT_TRUE(shv.isObject());
    auto *obj = static_cast<DummyObject *>(shv.getObject(runtime));
    ASSERT_TRUE(obj->hvBool.getBool());
  }

  lv.obj = nullptr;
  // Now lv.pshv is the only reference that holds the object alive.
  runtime.collect("test");
  {
    auto shv = lv.pshv.getSmallHermesValue();
    ASSERT_TRUE(shv.isObject());
    auto *obj = static_cast<testhelpers::DummyObject *>(shv.getObject(runtime));
    ASSERT_TRUE(obj->hvBool.getBool());
  }

  lv.str = createNoThrowForTest(runtime, "I'm a string");
  auto sym =
      runtime.getIdentifierTable().createNotUniquedSymbol(runtime, lv.str);
  ASSERT_FALSE(isException(sym));
  lv.sym = *sym;
  lv.pshv = SmallHermesValue::encodeSymbolValue(*lv.sym);
  runtime.collect("test");
  {
    auto shv = lv.pshv.getSmallHermesValue();
    ASSERT_TRUE(shv.isSymbol());
    ASSERT_TRUE(shv.getSymbol().isValid());
  }

  lv.sym = SymbolID{};
  runtime.collect("test");
  {
    auto shv = lv.pshv.getSmallHermesValue();
    ASSERT_TRUE(shv.isSymbol());
    ASSERT_TRUE(shv.getSymbol().isValid());
  }
}

#if defined(HERMES_SLOW_DEBUG) && defined(ASSERT_DEATH)
TEST_F(HandleDeathTest, UseFlushedHandle) {
  auto marker = gcScope.createMarker();
  auto handle = createNoThrowForTest(runtime, "hello");
  // This flush should destroy the handle created after the marker, using it
  // afterwards should trigger an assertion failure.
  gcScope.flushToMarker(marker);
  ASSERT_DEATH(
      { handle->getStringLength(); }, "Assertion.*Reading from flushed handle");
}

TEST_F(HandleDeathTest, UseRAIIFlushedHandle) {
  GCScopeMarkerRAII marker(gcScope);
  auto handle = createNoThrowForTest(runtime, "hello");
  // This flush should destroy the handle created after the marker, using it
  // afterwards should trigger an assertion failure.
  marker.flush();
  ASSERT_DEATH(
      { handle->getStringLength(); }, "Assertion.*Reading from flushed handle");
}

TEST_F(HandleDeathTest, FlushToSmallCount) {
  auto handle = createNoThrowForTest(runtime, "hello");
  // This flush should destroy the handle created after the marker, using it
  // afterwards should trigger an assertion failure.
  gcScope.flushToSmallCount(0);
  ASSERT_DEATH(
      { handle->getStringLength(); }, "Assertion.*Reading from flushed handle");
}
#endif

} // namespace

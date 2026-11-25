/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VMRuntimeTestHelpers.h"
#include "hermes/VM/DummyObject.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StringPrimitive.h"

#include "gtest/gtest.h"

#include <limits>

using namespace hermes::vm;
using namespace hermes::vm::testhelpers;

namespace {

using PinnedValueTest = RuntimeTestFixture;

/// Kept from the former HandleTest.cpp.
/// PinnedValue<SmallHermesValue> must keep objects and non-uniqued symbols
/// alive when it is the only root, and must be updated when they move.
TEST_F(PinnedValueTest, PinnedValueHV32Test) {
  struct : Locals {
    PinnedValue<DummyObject> obj;
    PinnedValue<SmallHermesValue> pshv;
    PinnedValue<StringPrimitive> str;
    PinnedValue<SymbolID> sym;
    PinnedValue<StringPrimitive> storage;
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

  // isValid() only checks the bit pattern, so it cannot detect a symbol whose
  // identifier table entry was freed. Read the string back through the table
  // instead: a freed entry is not a string primitive and trips an assert in
  // getStringView().
  const ASCIIRef kSymString = createASCIIRef("I'm a string");
  MutableHandle<StringPrimitive> storage{lv.storage};
  auto checkSymbolString = [&](SymbolID id) {
    ASSERT_TRUE(id.isValid());
    ASSERT_TRUE(id.isNotUniqued());
    EXPECT_TRUE(runtime.getIdentifierTable()
                    .getStringView(id, storage)
                    .equals(kSymString));
  };

  lv.str = StringPrimitive::createNoThrow(runtime, "I'm a string");
  auto sym =
      runtime.getIdentifierTable().createNotUniquedSymbol(runtime, lv.str);
  ASSERT_FALSE(isException(sym));
  lv.sym = *sym;
  // The symbol entry must be the only thing keeping the string alive.
  lv.str = nullptr;
  lv.pshv = SmallHermesValue::encodeSymbolValue(*lv.sym);
  runtime.collect("test");
  {
    auto shv = lv.pshv.getSmallHermesValue();
    ASSERT_TRUE(shv.isSymbol());
    checkSymbolString(shv.getSymbol());
  }

  lv.sym = SymbolID{};
  // Now lv.pshv is the only root of the symbol.
  runtime.collect("test");
  {
    auto shv = lv.pshv.getSmallHermesValue();
    ASSERT_TRUE(shv.isSymbol());
    checkSymbolString(shv.getSymbol());
  }
  // Allocate more symbols: if marking had failed and the entry above had been
  // freed, its slot would be reused here and the check below would fail.
  // (Lazy symbols are not freed by collection; each one takes a new slot.)
  for (int i = 0; i < 4; ++i) {
    runtime.getIdentifierTable().createNotUniquedLazySymbol(
        createASCIIRef("other"));
    runtime.collect("test");
  }
  checkSymbolString(lv.pshv.getSmallHermesValue().getSymbol());

#ifdef HERMESVM_BOXED_DOUBLES
  // A boxed double is a heap pointer; Locals must root and relocate it too.
  constexpr double kBoxedDouble = std::numeric_limits<double>::max();
  lv.pshv = SmallHermesValue::encodeNumberValue(kBoxedDouble, runtime);
  ASSERT_TRUE(lv.pshv.getSmallHermesValue().isBoxedDouble());
  runtime.collect("test");
  {
    auto shv = lv.pshv.getSmallHermesValue();
    ASSERT_TRUE(shv.isBoxedDouble());
    EXPECT_EQ(kBoxedDouble, shv.getNumber(runtime));
  }
#endif
}

/// Handle<T> and MutableHandle<T> are views of a PinnedValue slot: copies and
/// widening conversions alias the same slot, and a write through a
/// MutableHandle is visible through every view.
TEST_F(PinnedValueTest, HandleViewsAliasPinnedSlot) {
  struct : Locals {
    PinnedValue<StringPrimitive> str1;
    PinnedValue<StringPrimitive> str2;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  createNoThrowForTest(runtime, createUTF16Ref(u"str1"), lv.str1);
  createNoThrowForTest(runtime, createUTF16Ref(u"str2"), lv.str2);

  Handle<StringPrimitive> h1 = lv.str1; // PinnedValue -> Handle.
  Handle<StringPrimitive> h2(h1); // Copy.
  Handle<> generic = h1; // Widening conversion.
  ASSERT_EQ(*lv.str1, *h1);
  ASSERT_EQ(*h1, *h2);
  ASSERT_EQ(lv.str1.getHermesValue(), generic.getHermesValue());
  ASSERT_TRUE(h1 == h2);
  ASSERT_FALSE(h1 == Handle<StringPrimitive>(lv.str2));

  // Writes through a MutableHandle go to the underlying slot.
  MutableHandle<StringPrimitive> mut{lv.str1};
  mut.set(*lv.str2);
  ASSERT_EQ(*lv.str2, *lv.str1);
  ASSERT_EQ(*lv.str2, *h1);
  ASSERT_EQ(*lv.str2, *h2);

  // The views follow the string if it is moved.
  runtime.collect("test");
  EXPECT_EQ(4u, h1->getStringLength());
  ASSERT_EQ(*lv.str1, *mut);

  mut.clear();
  ASSERT_TRUE(*lv.str1 == nullptr);
  ASSERT_TRUE(*h1 == nullptr);
  // The original slot was not affected by clearing the other.
  EXPECT_EQ(4u, lv.str2->getStringLength());
}

/// Same as above for untyped Handle<>/MutableHandle<> holding non-pointer
/// values, including moving a MutableHandle.
TEST_F(PinnedValueTest, UntypedMutableHandleAliasesPinnedSlot) {
  struct : Locals {
    PinnedValue<> v1;
    PinnedValue<> v2;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.v1 = HermesValue::encodeTrustedNumberValue(1);
  lv.v2 = HermesValue::encodeTrustedNumberValue(2);

  MutableHandle<> h1{lv.v1};
  MutableHandle<> moved{std::move(h1)};
  Handle<> view = moved;
  ASSERT_EQ(1.0, view->getNumber());

  moved.set(lv.v2.getHermesValue());
  ASSERT_EQ(2.0, lv.v1.getHermesValue().getNumber());
  ASSERT_EQ(2.0, view->getNumber());

  moved = HermesValue::encodeTrustedNumberValue(3);
  ASSERT_EQ(3.0, lv.v1.getHermesValue().getNumber());
  ASSERT_EQ(2.0, lv.v2.getHermesValue().getNumber());
}

/// Handle<Derived> converts to Handle<Base>.
TEST_F(PinnedValueTest, HandleUpcast) {
  struct : Locals {
    PinnedValue<JSFunction> function;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  lv.function =
      JSFunction::create(runtime, domain, runtime.makeNullHandle<JSObject>());
  Handle<JSFunction> function = lv.function;
  Handle<JSObject> obj = function;
  ASSERT_EQ(function.get(), obj.get());
}

/// Moving a PseudoHandle into a PinnedValue roots the value.
TEST_F(PinnedValueTest, PseudoHandleMovedIntoPinnedValue) {
  struct : Locals {
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii{runtime, &lv};

  auto ph = StringPrimitive::createNoThrow(runtime, "pseudo");
  lv.str = std::move(ph);
  runtime.collect("test");
  EXPECT_EQ(6u, lv.str->getStringLength());
}

} // namespace

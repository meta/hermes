/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/StringView.h"
#include "hermes/VM/Runtime.h"

#include "VMRuntimeTestHelpers.h"

#include "gtest/gtest.h"

using namespace hermes::vm;

namespace {

using StringViewTest = RuntimeTestFixture;

TEST_F(StringViewTest, Construction) {
  // Test createStringView
  UTF16Ref str1 = createUTF16Ref(u"abcdef\x100");
  auto strPrim1 = createNoThrowForTest(runtime, str1);
  StringView view1 = StringPrimitive::createStringView(runtime, strPrim1);
  EXPECT_TRUE(!view1.isASCII());
  EXPECT_EQ(view1.length(), 7u);

  // Test getUTF16Ref and copyUTF16String.
  SmallU16String<8> storage;
  UTF16Ref ref1 = view1.getUTF16Ref(storage);
  // Does not need to use storage.
  EXPECT_TRUE(storage.empty());
  EXPECT_EQ(ref1.data(), view1.castToChar16Ptr());

  // After copy, storage should now contain data.
  view1.appendUTF16String(storage);
  EXPECT_EQ(storage.size(), 7u);
}

TEST_F(StringViewTest, Comparison) {
  auto strPrim1 = createNoThrowForTest(runtime, createUTF16Ref(u"abcde"));
  auto view1 = StringPrimitive::createStringView(runtime, strPrim1);

  auto strPrim2 = createNoThrowForTest(runtime, createUTF16Ref(u"abcdefg"));
  auto view2 = StringPrimitive::createStringView(runtime, strPrim2);

  EXPECT_FALSE(view1.equals(view2));
  EXPECT_FALSE(view2.equals(view1));

  auto strPrim3 = createNoThrowForTest(runtime, createUTF16Ref(u"abc"));
  auto view3 = StringPrimitive::createStringView(runtime, strPrim3);

  EXPECT_FALSE(view1.equals(view3));
  EXPECT_FALSE(view3.equals(view1));

  auto strPrim4 = createNoThrowForTest(runtime, createUTF16Ref(u"defgh"));
  auto view4 = StringPrimitive::createStringView(runtime, strPrim4);
  EXPECT_FALSE(view1.equals(view4));
  EXPECT_FALSE(view4.equals(view1));

  llvh::StringRef str = "abcde";
  auto strPrim5 = createNoThrowForTest(runtime, str);
  auto view5 = StringPrimitive::createStringView(runtime, strPrim5);
  EXPECT_TRUE(view1.equals(view5));
  EXPECT_TRUE(view1.equals(view5));

  // Test copy ctor and operator=.
  StringView tmp = view1;
  EXPECT_TRUE(tmp.equals(view1));
  tmp = view2;
  EXPECT_FALSE(tmp.equals(view1));
  tmp = view1;
  EXPECT_TRUE(tmp.equals(view1));
}

TEST_F(StringViewTest, HandleHandling) {
  // StringView conditionally holds onto a handle.
  // This verifies that we correctly perform handle book-keeping in ctors and
  // operator=. It has no assertions; it relies on GCScope to check its handle
  // count in its dtor.
  GCScope scope1{runtime, "StringViewTest.HandleHandling1"};

  auto strPrim1 =
      createNoThrowForTest(runtime, createUTF16Ref(u"I have a handle"));
  StringView scope1View1 = StringPrimitive::createStringView(runtime, strPrim1);
  StringView scope1View2 = StringPrimitive::createStringView(runtime, strPrim1);
  StringView scope1View3 = StringPrimitive::createStringView(runtime, strPrim1);
  (void)scope1View3;

  GCScope scope2{runtime, "StringViewTest.HandleHandling2"};
  auto strPrim2 =
      createNoThrowForTest(runtime, createUTF16Ref(u"I also have a handle"));
  StringView scope2View1 = StringPrimitive::createStringView(runtime, strPrim2);

  // This assignment requires us to change the GCScope associated with the
  // handle in scope1.
  StringView tmp1 = scope2View1;
  tmp1 = scope1View1;

  // This ctor ensures we bump the handle count in scope1 and decrement it
  // scope2.
  StringView tmp2 = scope1View1;
  (void)tmp2;

  // Same but with moves ctor and operator=.
  StringView tmp3 = std::move(scope1View2);
  (void)tmp3;
  StringView tmp4 = scope2View1;
  tmp4 = std::move(scope1View2);
}

TEST_F(StringViewTest, Iteration) {
  auto str1 = createUTF16Ref(u"abcdefghijklmnopqrstuvwxyz");
  auto strPrim1 = createNoThrowForTest(runtime, str1);
  auto view1 = StringPrimitive::createStringView(runtime, strPrim1);
  uint32_t index = 0;
  for (const char16_t c : view1) {
    EXPECT_EQ(c, str1[index++]);
  }
}

TEST_F(StringViewTest, Slice) {
  auto str = createUTF16Ref(u"abcdefgh");
  auto strPrim1 = createNoThrowForTest(runtime, str);
  auto view = StringPrimitive::createStringView(runtime, strPrim1);
  auto itr = view.begin();
  for (uint32_t i = 0; i < 8; ++i, ++itr) {
    EXPECT_TRUE(view.slice(i).equals(str.slice(i)));
    EXPECT_TRUE(view.slice(i, 8 - i).equals(str.slice(i, 8 - i)));
    EXPECT_TRUE(view.slice(itr, view.end()).equals(str.slice(i)));
  }
}

TEST_F(StringViewTest, Output) {
  auto str = createUTF16Ref(u"abcd");
  llvh::SmallVector<char, 32> result{};
  llvh::raw_svector_ostream os{result};
  auto strPrim = createNoThrowForTest(runtime, str);
  auto view = StringPrimitive::createStringView(runtime, strPrim);
  os << view;
  EXPECT_EQ('a', result[0]);
  EXPECT_EQ('b', result[1]);
  EXPECT_EQ('c', result[2]);
  EXPECT_EQ('d', result[3]);
}

} // namespace

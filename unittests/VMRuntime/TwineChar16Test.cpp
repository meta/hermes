/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/TwineChar16.h"

#include "gtest/gtest.h"

#include "VMRuntimeTestHelpers.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StringPrimitive.h"

using namespace hermes::vm;

namespace {

#define EXPECT_TWINE_OUTPUT(expectedSize, expected, twine)            \
  do {                                                                \
    EXPECT_EQ(expectedSize, (twine).size());                          \
    auto vec = llvh::SmallVector<char16_t, 32>();                     \
    auto utf16Ref = createUTF16Ref(expected);                         \
    (twine).toVector(vec);                                            \
    EXPECT_EQ(utf16Ref, UTF16Ref(vec));                               \
    char16_t arr[64] = {u'\0'};                                       \
    EXPECT_EQ((twine).size(), (twine).toChar16Str(arr, sizeof(arr))); \
    EXPECT_EQ(utf16Ref, createUTF16Ref(arr));                         \
  } while (0)

using TwineChar16Test = RuntimeTestFixture;

TEST_F(TwineChar16Test, ConstructTest) {
  EXPECT_TWINE_OUTPUT(0u, u"", TwineChar16::createNull());
  EXPECT_TWINE_OUTPUT(4u, u"abcd", TwineChar16("abcd"));
  EXPECT_TWINE_OUTPUT(4u, u"abcd", TwineChar16(u"abcd"));
  EXPECT_TWINE_OUTPUT(4u, u"abcd", TwineChar16(createUTF16Ref(u"abcd")));
  EXPECT_TWINE_OUTPUT(1u, u"0", TwineChar16(0));
  EXPECT_TWINE_OUTPUT(5u, u"12352", TwineChar16(12352));

  /// Ensure the const char * constructor works.
  auto sink =
      [](size_t size, const char16_t *expected, const TwineChar16 &twine) {
        EXPECT_TWINE_OUTPUT(size, expected, twine);
      };
  sink(4u, u"abcd", "abcd");

  {
    EXPECT_TWINE_OUTPUT(
        4u, u"abcd", TwineChar16(createNoThrowForTest(runtime, "abcd").get()));
  }
}

TEST_F(TwineChar16Test, ConcatTest) {
  EXPECT_TWINE_OUTPUT(4u, u"abcd", TwineChar16(u"ab", u"cd"));
  EXPECT_TWINE_OUTPUT(2u, u"ab", TwineChar16(u"ab", u""));
  EXPECT_TWINE_OUTPUT(2u, u"ab", TwineChar16(u"", u"ab"));
  EXPECT_TWINE_OUTPUT(2u, u"ab", TwineChar16(u"ab", ""));
  EXPECT_TWINE_OUTPUT(2u, u"ab", TwineChar16("ab", u""));
  EXPECT_TWINE_OUTPUT(
      4u, u"abcd", TwineChar16(createUTF16Ref(u"ab"), createUTF16Ref(u"cd")));
  EXPECT_TWINE_OUTPUT(4u, u"abcd", TwineChar16(u"ab", createUTF16Ref(u"cd")));
  EXPECT_TWINE_OUTPUT(6u, u"ab1842", TwineChar16("ab", 1842));
  EXPECT_TWINE_OUTPUT(6u, u"ab1842", llvh::StringRef("ab") + 1842);
  EXPECT_TWINE_OUTPUT(4u, u"abcd", TwineChar16(createUTF16Ref(u"ab"), u"cd"));
  EXPECT_TWINE_OUTPUT(
      8u,
      u"abcdefgh",
      TwineChar16(u"ab") + "cd" + u"ef" + createUTF16Ref(u"gh"));
  EXPECT_TWINE_OUTPUT(
      4u, u"abcd", TwineChar16(u"ab") + TwineChar16(createUTF16Ref(u"cd")));
  EXPECT_TWINE_OUTPUT(4u, u"abcd", "ab" + TwineChar16("cd"));

  {
    EXPECT_TWINE_OUTPUT(
        8u,
        u"abcdefgh",
        ((TwineChar16(u"ab") + TwineChar16(createUTF16Ref(u"cd"))) +
         (TwineChar16("ef") +
          TwineChar16(createNoThrowForTest(runtime, "gh").get()))));
  }

  {
#define TREE_TWINE                           \
  ((TwineChar16("ab") + TwineChar16("cd")) + \
   (TwineChar16("ef") + TwineChar16("gh")))

    EXPECT_TWINE_OUTPUT(8u, u"abcdefgh", TREE_TWINE);

    char16_t arr1[64] = {u'\0'};
    TREE_TWINE.toChar16Str(arr1, 4);
    EXPECT_EQ(createUTF16Ref(u"abcd"), createUTF16Ref(arr1));

    TREE_TWINE.toChar16Str(arr1, 7);
    EXPECT_EQ(createUTF16Ref(u"abcdefg"), createUTF16Ref(arr1));

    TREE_TWINE.toChar16Str(arr1, 50);
    EXPECT_EQ(createUTF16Ref(u"abcdefgh"), createUTF16Ref(arr1));
  }
}
} // namespace

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VMRuntimeTestHelpers.h"

#include "hermes/ADT/SafeInt.h"
#include "hermes/VM/JSLib/Base64Util.h"
#include "hermes/VM/StringBuilder.h"

using namespace hermes::vm;

namespace {

using Base64UtilTest = RuntimeTestFixture;

#define EXPECT_ENCODED(original, expected)                                  \
  {                                                                         \
    uint64_t expectedLength = ((original.size() + 2) / 3) * 4;              \
    EXPECT_LE(expectedLength, std::numeric_limits<uint32_t>::max());        \
    hermes::SafeUInt32 outputLength{static_cast<uint32_t>(expectedLength)}; \
    struct : public Locals {                                                \
      PinnedValue<StringPrimitive> storageSlot;                             \
    } storageLv;                                                            \
    LocalsRAII storageLraii(runtime, &storageLv);                           \
    MutableHandle<StringPrimitive> storage{storageLv.storageSlot};          \
    CallResult<StringBuilder> builder = StringBuilder::createStringBuilder( \
        runtime, storage, outputLength, true);                              \
    EXPECT_NE(builder, ExecutionStatus::EXCEPTION);                         \
                                                                            \
    bool success = base64Encode(original, *builder);                        \
    EXPECT_TRUE(success);                                                   \
    EXPECT_EQ(                                                              \
        builder->getStringPrimitive()->getStringRef<char>(),                \
        createASCIIRef(expected));                                          \
  }

#define EXPECT_ENCODED_ASCII_AND_UTF16(original, expected)      \
  {                                                             \
    ASCIIRef asciiRef = createASCIIRef(original);               \
    EXPECT_ENCODED(asciiRef, expected);                         \
                                                                \
    std::vector<char16_t> converted(asciiRef.size() + 1);       \
    uint32_t i = 0;                                             \
    for (i = 0; i < asciiRef.size(); i++) {                     \
      converted[i] = asciiRef[i];                               \
    }                                                           \
    converted[i] = '\0';                                        \
    EXPECT_ENCODED(createUTF16Ref(converted.data()), expected); \
  }

#define EXPECT_DECODED(original, expected)                                  \
  {                                                                         \
    hermes::OptValue<uint32_t> expectedLength =                             \
        base64DecodeOutputLength(original);                                 \
    EXPECT_TRUE(expectedLength.hasValue());                                 \
    struct : public Locals {                                                \
      PinnedValue<StringPrimitive> storageSlot;                             \
    } storageLv;                                                            \
    LocalsRAII storageLraii(runtime, &storageLv);                           \
    MutableHandle<StringPrimitive> storage{storageLv.storageSlot};          \
    CallResult<StringBuilder> builder = StringBuilder::createStringBuilder( \
        runtime, storage, hermes::SafeUInt32(*expectedLength), false);      \
    EXPECT_NE(builder, ExecutionStatus::EXCEPTION);                         \
                                                                            \
    bool success = base64Decode(original, *builder);                        \
    EXPECT_TRUE(success);                                                   \
    EXPECT_EQ(                                                              \
        builder->getStringPrimitive()->getStringRef<char16_t>(),            \
        createUTF16Ref(expected));                                          \
  }

#define EXPECT_DECODED_ASCII_AND_UTF16(original, expected)      \
  {                                                             \
    ASCIIRef asciiRef = createASCIIRef(original);               \
    EXPECT_DECODED(asciiRef, expected);                         \
                                                                \
    std::vector<char16_t> converted(asciiRef.size() + 1);       \
    uint32_t i = 0;                                             \
    for (i = 0; i < asciiRef.size(); i++) {                     \
      converted[i] = asciiRef[i];                               \
    }                                                           \
    converted[i] = '\0';                                        \
    EXPECT_DECODED(createUTF16Ref(converted.data()), expected); \
  }

TEST_F(Base64UtilTest, EdgeCases) {
  EXPECT_ENCODED_ASCII_AND_UTF16("", "");
}

TEST_F(Base64UtilTest, EncodePaddingRequired) {
  EXPECT_ENCODED_ASCII_AND_UTF16("a", "YQ==");
  EXPECT_ENCODED_ASCII_AND_UTF16("ab", "YWI=");
  EXPECT_ENCODED_ASCII_AND_UTF16("abcd", "YWJjZA==");
  EXPECT_ENCODED_ASCII_AND_UTF16("abcde", "YWJjZGU=");
  EXPECT_ENCODED_ASCII_AND_UTF16(
      "less is more than more", "bGVzcyBpcyBtb3JlIHRoYW4gbW9yZQ==");
  EXPECT_ENCODED_ASCII_AND_UTF16("<>?su", "PD4/c3U=");

  EXPECT_ENCODED(UTF16Ref(std::array<char16_t, 1>{1}), "AQ==");
  EXPECT_ENCODED(ASCIIRef(std::array<char, 1>{1}), "AQ==");
  EXPECT_ENCODED(UTF16Ref(std::array<char16_t, 2>{1, 0}), "AQA=");
  EXPECT_ENCODED(ASCIIRef(std::array<char, 2>{1, 0}), "AQA=");
}

TEST_F(Base64UtilTest, EncodePaddingNotNeeded) {
  EXPECT_ENCODED_ASCII_AND_UTF16("abc", "YWJj");
  EXPECT_ENCODED_ASCII_AND_UTF16("abcdef", "YWJjZGVm");

  EXPECT_ENCODED(UTF16Ref(std::array<char16_t, 3>{0, 0, 0}), "AAAA");
  EXPECT_ENCODED(ASCIIRef(std::array<char, 3>{0, 0, 0}), "AAAA");
  EXPECT_ENCODED(UTF16Ref(std::array<char16_t, 3>{1, 0, 0}), "AQAA");
  EXPECT_ENCODED(ASCIIRef(std::array<char, 3>{1, 0, 0}), "AQAA");
}

TEST_F(Base64UtilTest, EncodeInvalid) {
  struct : public Locals {
    PinnedValue<StringPrimitive> storageSlot;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  // Just a long enough buffer. All calls in this function are expected to fail.
  hermes::SafeUInt32 outputLength{20};
  MutableHandle<StringPrimitive> storage{lv.storageSlot};
  CallResult<StringBuilder> builder =
      StringBuilder::createStringBuilder(runtime, storage, outputLength, true);
  EXPECT_NE(builder, ExecutionStatus::EXCEPTION);
  EXPECT_FALSE(base64Encode(createUTF16Ref(u"\U0001F600"), *builder));
  EXPECT_FALSE(base64Encode(createUTF16Ref(u"a\U0001F600"), *builder));
  EXPECT_FALSE(base64Encode(createUTF16Ref(u"ab\U0001F600"), *builder));
  EXPECT_FALSE(base64Encode(createUTF16Ref(u"abc\U0001F600"), *builder));
  EXPECT_FALSE(base64Encode(createUTF16Ref(u"\U0001F600xyz"), *builder));
  EXPECT_FALSE(base64Encode(createUTF16Ref(u"abc\U0001F600xyz"), *builder));
}

TEST_F(Base64UtilTest, DecodeValid) {
  EXPECT_DECODED_ASCII_AND_UTF16("", u"");
  EXPECT_DECODED_ASCII_AND_UTF16("YQ==", u"a");
  EXPECT_DECODED_ASCII_AND_UTF16("YR==", u"a");
  EXPECT_DECODED_ASCII_AND_UTF16("YWI=", u"ab");
  EXPECT_DECODED_ASCII_AND_UTF16("YWJj", u"abc");
  EXPECT_DECODED_ASCII_AND_UTF16("YWJjZA==", u"abcd");
  EXPECT_DECODED_ASCII_AND_UTF16("YWJjZA", u"abcd");
  EXPECT_DECODED_ASCII_AND_UTF16("YWJjZGU=", u"abcde");
  EXPECT_DECODED_ASCII_AND_UTF16("YWJjZGU", u"abcde");
  EXPECT_DECODED_ASCII_AND_UTF16("YWJjZGVm", u"abcdef");
  EXPECT_DECODED_ASCII_AND_UTF16("0w==", u"\xD3");
  EXPECT_DECODED_ASCII_AND_UTF16("000=", u"\xD3M");
}

TEST_F(Base64UtilTest, DecodeWithWhitespace) {
  EXPECT_DECODED_ASCII_AND_UTF16("      ", u"");
  EXPECT_DECODED_ASCII_AND_UTF16("\x09\x0A\x0C\x0D\x20", u"");
  EXPECT_DECODED_ASCII_AND_UTF16("Y Q = =", u"a");
  EXPECT_DECODED_ASCII_AND_UTF16("\x09Y\x0AQ\x0C=\x0D=\x20", u"a");
  EXPECT_DECODED_ASCII_AND_UTF16("   YR==", u"a");
  EXPECT_DECODED_ASCII_AND_UTF16("YR==    ", u"a");
}

TEST_F(Base64UtilTest, DecodeInvalid) {
  struct : public Locals {
    PinnedValue<StringPrimitive> storageSlot;
  } lv;
  LocalsRAII lraii(runtime, &lv);
  // Just a long enough buffer. All calls in this function are expected to fail.
  hermes::SafeUInt32 outputLength{50};
  MutableHandle<StringPrimitive> storage{lv.storageSlot};
  CallResult<StringBuilder> builder =
      StringBuilder::createStringBuilder(runtime, storage, outputLength, false);

  std::array<char, 5> hasNegative = {'A', 'b', 'c', (char)-15, '\0'};
  EXPECT_FALSE(base64Decode(createASCIIRef(hasNegative.data()), *builder));

  EXPECT_FALSE(base64Decode(createASCIIRef("==="), *builder));
  EXPECT_FALSE(base64Decode(createASCIIRef("0==="), *builder));
  EXPECT_FALSE(base64Decode(createASCIIRef("aa=0"), *builder));
  EXPECT_FALSE(
      base64Decode(createASCIIRef("$aaaaaaaaaaaaaaaaaaaaaaa"), *builder));
  EXPECT_FALSE(
      base64Decode(createASCIIRef("aaaaaa$aaaaaaaaaaaaaaaaa"), *builder));
  EXPECT_FALSE(base64Decode(
      createASCIIRef("bbbbbbbbbddddddddddddddddddddaaaaaaadddddddb="),
      *builder));

  // Strings that are the wrong length to be a valid base64 encoded string
  EXPECT_FALSE(base64Decode(createASCIIRef("A"), *builder));
  EXPECT_FALSE(base64Decode(createASCIIRef("B="), *builder));
  EXPECT_FALSE(base64Decode(createASCIIRef("ba="), *builder));
  EXPECT_FALSE(base64Decode(createASCIIRef("aaaaa"), *builder));

  // Non-Base64 ASCII , i.e., not [a-z]|[A-Z]|[0-p]|\+|\/
  EXPECT_FALSE(base64Decode(createASCIIRef("a*"), *builder));
  EXPECT_FALSE(base64Decode(createASCIIRef("YQ*"), *builder));

  // Out of ASCII range.
  EXPECT_FALSE(base64Decode(createASCIIRef("a\xFF"), *builder));

  // Padding where it's not expected
  EXPECT_FALSE(base64Decode(createASCIIRef("="), *builder));
  EXPECT_FALSE(base64Decode(createASCIIRef("=="), *builder));

  // Padding in the middle
  EXPECT_FALSE(base64Decode(createASCIIRef("YQ==YQ=="), *builder));

  // Extra padding
  EXPECT_FALSE(base64Decode(createASCIIRef("YQ==="), *builder));

  EXPECT_FALSE(base64Decode(createUTF16Ref(u"a\uff20=="), *builder));
  EXPECT_FALSE(
      base64Decode(createUTF16Ref(u"\u0065\u0065\u0065\u03A9"), *builder));
}

} // end anonymous namespace

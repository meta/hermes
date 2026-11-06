/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NapiTestFixture.h"

#include "hermes/VM/HermesValue.h"
#include "hermes/VM/StringPrimitive.h"

#include <cstring>
#include <string>

namespace {

using hermes::napi::NapiTestFixture;
using namespace hermes::vm;

/// Helper to open a handle scope and return the scope handle, asserting
/// success.
static napi_handle_scope openScope(napi_env env) {
  napi_handle_scope scope = nullptr;
  EXPECT_EQ(napi_ok, napi_open_handle_scope(env, &scope));
  EXPECT_NE(nullptr, scope);
  return scope;
}

/// Helper to close a handle scope, asserting success.
static void closeScope(napi_env env, napi_handle_scope scope) {
  EXPECT_EQ(napi_ok, napi_close_handle_scope(env, scope));
}

/// Helper to extract the StringPrimitive content as a UTF-16 vector
/// from a napi_value that is known to be a string.
static std::u16string getStringContent(napi_value val) {
  auto *phv = reinterpret_cast<PinnedHermesValue *>(val);
  auto *str = vmcast<StringPrimitive>(*phv);
  llvh::SmallVector<char16_t, 64> buf;
  str->appendUTF16String(buf);
  return std::u16string(buf.begin(), buf.end());
}

//===========================================================================
// napi_create_string_utf8 — basic creation
//===========================================================================

TEST_F(NapiTestFixture, CreateStringUtf8_EmptyString) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "", 0, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(0u, str->getStringLength());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf8_SimpleASCII) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(5u, str->getStringLength());
  EXPECT_TRUE(str->isASCII());

  // Verify content.
  auto ref = str->getStringRef<char>();
  EXPECT_EQ(0, std::memcmp(ref.data(), "hello", 5));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf8_AutoLength) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_create_string_utf8(env_, "world", NAPI_AUTO_LENGTH, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(5u, str->getStringLength());
  EXPECT_TRUE(str->isASCII());

  auto ref = str->getStringRef<char>();
  EXPECT_EQ(0, std::memcmp(ref.data(), "world", 5));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf8_SingleChar) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "A", 1, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(1u, str->getStringLength());
  EXPECT_EQ(u'A', str->at(0));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf8_MultibyteUTF8) {
  napi_handle_scope scope = openScope(env_);

  // UTF-8 encoding of "café": c=0x63 a=0x61 f=0x66 é=0xC3 0xA9
  const char *utf8 = "caf\xC3\xA9";
  size_t utf8Len = 5; // 5 bytes

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, utf8, utf8Len, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto *str = vmcast<StringPrimitive>(*phv);
  // "café" is 4 UTF-16 code units (é is U+00E9, single code unit).
  EXPECT_EQ(4u, str->getStringLength());

  auto content = getStringContent(result);
  EXPECT_EQ(u"caf\u00E9", content);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf8_ThreeByteUTF8) {
  napi_handle_scope scope = openScope(env_);

  // U+2603 SNOWMAN = 0xE2 0x98 0x83 in UTF-8
  const char *utf8 = "\xE2\x98\x83";
  size_t utf8Len = 3;

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, utf8, utf8Len, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(1u, str->getStringLength());

  auto content = getStringContent(result);
  EXPECT_EQ(u"\u2603", content);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf8_FourByteUTF8_Emoji) {
  napi_handle_scope scope = openScope(env_);

  // U+1F600 GRINNING FACE = 0xF0 0x9F 0x98 0x80 in UTF-8
  // In UTF-16, this is a surrogate pair: 0xD83D 0xDE00
  const char *utf8 = "\xF0\x9F\x98\x80";
  size_t utf8Len = 4;

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, utf8, utf8Len, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto *str = vmcast<StringPrimitive>(*phv);
  // Surrogate pair: 2 UTF-16 code units.
  EXPECT_EQ(2u, str->getStringLength());

  auto content = getStringContent(result);
  EXPECT_EQ(u'\xD83D', content[0]);
  EXPECT_EQ(u'\xDE00', content[1]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf8_LongerString) {
  napi_handle_scope scope = openScope(env_);

  std::string longStr(256, 'x');
  napi_value result = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_create_string_utf8(env_, longStr.c_str(), longStr.size(), &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(256u, str->getStringLength());
  EXPECT_TRUE(str->isASCII());

  auto ref = str->getStringRef<char>();
  EXPECT_EQ(longStr, std::string(ref.data(), ref.size()));

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_string_utf8 — typeof
//===========================================================================

TEST_F(NapiTestFixture, CreateStringUtf8_TypeofIsString) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "test", 4, &result));

  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, result, &type));
  EXPECT_EQ(napi_string, type);

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_string_utf8 — GC safety
//===========================================================================

TEST_F(NapiTestFixture, CreateStringUtf8_SurvivesGC) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &result));

  // Trigger GC — the string should survive because it's in a handle
  // scope slot which is a GC root.
  collectAndDrain();

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(5u, str->getStringLength());

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_string_utf8 — argument validation
//===========================================================================

TEST_F(NapiTestFixture, CreateStringUtf8_NullEnv) {
  EXPECT_EQ(
      napi_invalid_arg, napi_create_string_utf8(nullptr, "hello", 5, nullptr));
}

TEST_F(NapiTestFixture, CreateStringUtf8_NullResult) {
  EXPECT_EQ(
      napi_invalid_arg, napi_create_string_utf8(env_, "hello", 5, nullptr));
}

TEST_F(NapiTestFixture, CreateStringUtf8_NullStrWithZeroLength) {
  napi_handle_scope scope = openScope(env_);

  // Null str pointer with length 0 should create an empty string.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, nullptr, 0, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(0u, str->getStringLength());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf8_NullStrWithNonZeroLength) {
  // Null str pointer with non-zero length should fail.
  napi_value result = nullptr;
  EXPECT_EQ(
      napi_invalid_arg, napi_create_string_utf8(env_, nullptr, 5, &result));
}

TEST_F(NapiTestFixture, CreateStringUtf8_ClearsError) {
  napi_handle_scope scope = openScope(env_);

  // Set an error state first.
  napi_set_last_error(env_, napi_generic_failure);
  EXPECT_EQ(napi_generic_failure, env_->last_error.error_code);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "test", 4, &result));
  EXPECT_EQ(napi_ok, env_->last_error.error_code);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf8_MultipleStringsInScope) {
  napi_handle_scope scope = openScope(env_);

  napi_value s1 = nullptr;
  napi_value s2 = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "foo", 3, &s1));
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "bar", 3, &s2));

  // Different handle scope slots.
  EXPECT_NE(s1, s2);

  // Both should be valid strings with correct content.
  auto content1 = getStringContent(s1);
  auto content2 = getStringContent(s2);
  EXPECT_EQ(u"foo", content1);
  EXPECT_EQ(u"bar", content2);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf8_EmptyWithAutoLength) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_string_utf8(env_, "", NAPI_AUTO_LENGTH, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(0u, str->getStringLength());

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_string_utf8 — invalid UTF-8 (U+FFFD replacement)
//===========================================================================

TEST_F(NapiTestFixture, CreateStringUtf8_InvalidContinuationByte) {
  napi_handle_scope scope = openScope(env_);

  // 0xC3 expects a continuation byte, but 0x28 is ASCII '('.
  // V8 replaces the invalid sequence with U+FFFD.
  const char input[] = "\xC3\x28";
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, input, 2, &result));
  ASSERT_NE(nullptr, result);

  auto content = getStringContent(result);
  // Expect U+FFFD followed by '('.
  ASSERT_EQ(2u, content.size());
  EXPECT_EQ(u'\xFFFD', content[0]);
  EXPECT_EQ(u'(', content[1]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf8_TruncatedTwoByte) {
  napi_handle_scope scope = openScope(env_);

  // 0xC3 starts a 2-byte sequence but is at end of input.
  const char input[] = "\xC3";
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, input, 1, &result));
  ASSERT_NE(nullptr, result);

  auto content = getStringContent(result);
  ASSERT_EQ(1u, content.size());
  EXPECT_EQ(u'\xFFFD', content[0]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf8_TruncatedThreeByte) {
  napi_handle_scope scope = openScope(env_);

  // 0xE2 starts a 3-byte sequence but only 2 bytes provided.
  const char input[] = "\xE2\x98";
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, input, 2, &result));
  ASSERT_NE(nullptr, result);

  auto content = getStringContent(result);
  ASSERT_EQ(1u, content.size());
  EXPECT_EQ(u'\xFFFD', content[0]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf8_TruncatedAtLengthNotTerminated) {
  napi_handle_scope scope = openScope(env_);

  // Only the first byte of each sequence is inside the given length. The
  // bytes after it must not be read, even though they would complete it.
  const char twoByte[] = "\xC3\xA9";
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, twoByte, 1, &result));
  auto content = getStringContent(result);
  ASSERT_EQ(1u, content.size());
  EXPECT_EQ(u'\xFFFD', content[0]);

  const char fourByte[] = "\xF0\x9F\x98\x80";
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, fourByte, 3, &result));
  content = getStringContent(result);
  ASSERT_EQ(1u, content.size());
  EXPECT_EQ(u'\xFFFD', content[0]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf8_StrayHighByte) {
  napi_handle_scope scope = openScope(env_);

  // 0xFF is never valid in UTF-8.
  const char input[] =
      "a\xFF"
      "b";
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, input, 3, &result));
  ASSERT_NE(nullptr, result);

  auto content = getStringContent(result);
  ASSERT_EQ(3u, content.size());
  EXPECT_EQ(u'a', content[0]);
  EXPECT_EQ(u'\xFFFD', content[1]);
  EXPECT_EQ(u'b', content[2]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf8_InvalidMixedWithValid) {
  napi_handle_scope scope = openScope(env_);

  // "hello" + invalid byte 0x80 (stray continuation) + "world"
  const char input[] = "hello\x80world";
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, input, 11, &result));
  ASSERT_NE(nullptr, result);

  auto content = getStringContent(result);
  // "hello" (5) + U+FFFD (1) + "world" (5) = 11
  ASSERT_EQ(11u, content.size());
  EXPECT_EQ(u"hello", content.substr(0, 5));
  EXPECT_EQ(u'\xFFFD', content[5]);
  EXPECT_EQ(u"world", content.substr(6));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf8_OverlongEncoding) {
  napi_handle_scope scope = openScope(env_);

  // Overlong encoding of '/' (U+002F): 0xC0 0xAF
  // Must be rejected and replaced with U+FFFD.
  const char input[] = "\xC0\xAF";
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, input, 2, &result));
  ASSERT_NE(nullptr, result);

  auto content = getStringContent(result);
  // The overlong sequence should be replaced.
  // At least one U+FFFD expected; exact count depends on
  // whether the engine treats it as 1 or 2 bad bytes.
  EXPECT_FALSE(content.empty());
  for (auto ch : content) {
    EXPECT_EQ(u'\xFFFD', ch);
  }

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_string_utf16 — basic creation
//===========================================================================

TEST_F(NapiTestFixture, CreateStringUtf16_EmptyString) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, u"", 0, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(0u, str->getStringLength());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf16_SimpleASCII) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, u"hello", 5, &result));
  ASSERT_NE(nullptr, result);

  auto content = getStringContent(result);
  EXPECT_EQ(u"hello", content);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf16_AutoLength) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_create_string_utf16(env_, u"world", NAPI_AUTO_LENGTH, &result));
  ASSERT_NE(nullptr, result);

  auto content = getStringContent(result);
  EXPECT_EQ(u"world", content);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf16_SingleChar) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, u"A", 1, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(1u, str->getStringLength());
  EXPECT_EQ(u'A', str->at(0));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf16_NonASCII) {
  napi_handle_scope scope = openScope(env_);

  // "café" in UTF-16: c=0x0063 a=0x0061 f=0x0066 é=0x00E9
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, u"caf\u00E9", 4, &result));
  ASSERT_NE(nullptr, result);

  auto content = getStringContent(result);
  EXPECT_EQ(u"caf\u00E9", content);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf16_SurrogatePair) {
  napi_handle_scope scope = openScope(env_);

  // U+1F600 GRINNING FACE as surrogate pair: 0xD83D 0xDE00
  const char16_t emoji[] = {0xD83D, 0xDE00, 0};
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, emoji, 2, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(2u, str->getStringLength());

  auto content = getStringContent(result);
  EXPECT_EQ(u'\xD83D', content[0]);
  EXPECT_EQ(u'\xDE00', content[1]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf16_BMP) {
  napi_handle_scope scope = openScope(env_);

  // U+2603 SNOWMAN
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, u"\u2603", 1, &result));
  ASSERT_NE(nullptr, result);

  auto content = getStringContent(result);
  EXPECT_EQ(u"\u2603", content);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf16_LongerString) {
  napi_handle_scope scope = openScope(env_);

  std::u16string longStr(256, u'x');
  napi_value result = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_create_string_utf16(env_, longStr.c_str(), longStr.size(), &result));
  ASSERT_NE(nullptr, result);

  auto content = getStringContent(result);
  EXPECT_EQ(longStr, content);

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_string_utf16 — typeof
//===========================================================================

TEST_F(NapiTestFixture, CreateStringUtf16_TypeofIsString) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, u"test", 4, &result));

  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, result, &type));
  EXPECT_EQ(napi_string, type);

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_string_utf16 — GC safety
//===========================================================================

TEST_F(NapiTestFixture, CreateStringUtf16_SurvivesGC) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, u"hello", 5, &result));

  // Trigger GC — the string should survive because it's in a handle
  // scope slot which is a GC root.
  collectAndDrain();

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(5u, str->getStringLength());

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_string_utf16 — argument validation
//===========================================================================

TEST_F(NapiTestFixture, CreateStringUtf16_NullEnv) {
  EXPECT_EQ(
      napi_invalid_arg,
      napi_create_string_utf16(nullptr, u"hello", 5, nullptr));
}

TEST_F(NapiTestFixture, CreateStringUtf16_NullResult) {
  EXPECT_EQ(
      napi_invalid_arg, napi_create_string_utf16(env_, u"hello", 5, nullptr));
}

TEST_F(NapiTestFixture, CreateStringUtf16_NullStrWithZeroLength) {
  napi_handle_scope scope = openScope(env_);

  // Null str pointer with length 0 should create an empty string.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, nullptr, 0, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(0u, str->getStringLength());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf16_NullStrWithNonZeroLength) {
  // Null str pointer with non-zero length should fail.
  napi_value result = nullptr;
  EXPECT_EQ(
      napi_invalid_arg, napi_create_string_utf16(env_, nullptr, 5, &result));
}

TEST_F(NapiTestFixture, CreateStringUtf16_ClearsError) {
  napi_handle_scope scope = openScope(env_);

  // Set an error state first.
  napi_set_last_error(env_, napi_generic_failure);
  EXPECT_EQ(napi_generic_failure, env_->last_error.error_code);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, u"test", 4, &result));
  EXPECT_EQ(napi_ok, env_->last_error.error_code);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf16_MultipleStringsInScope) {
  napi_handle_scope scope = openScope(env_);

  napi_value s1 = nullptr;
  napi_value s2 = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, u"foo", 3, &s1));
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, u"bar", 3, &s2));

  // Different handle scope slots.
  EXPECT_NE(s1, s2);

  // Both should be valid strings with correct content.
  auto content1 = getStringContent(s1);
  auto content2 = getStringContent(s2);
  EXPECT_EQ(u"foo", content1);
  EXPECT_EQ(u"bar", content2);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringUtf16_EmptyWithAutoLength) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_string_utf16(env_, u"", NAPI_AUTO_LENGTH, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(0u, str->getStringLength());

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_string_utf16 — roundtrip with UTF-8
//===========================================================================

TEST_F(NapiTestFixture, CreateStringUtf16_RoundtripWithUtf8Creation) {
  napi_handle_scope scope = openScope(env_);

  // Create a string via UTF-16 that includes non-ASCII characters.
  // "café" in UTF-16
  napi_value utf16Result = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_string_utf16(env_, u"caf\u00E9", 4, &utf16Result));

  // Create the same string via UTF-8.
  napi_value utf8Result = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_string_utf8(env_, "caf\xC3\xA9", 5, &utf8Result));

  // Both should have the same content.
  auto content16 = getStringContent(utf16Result);
  auto content8 = getStringContent(utf8Result);
  EXPECT_EQ(content16, content8);

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_string_latin1 — basic creation
//===========================================================================

TEST_F(NapiTestFixture, CreateStringLatin1_EmptyString) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_latin1(env_, "", 0, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(0u, str->getStringLength());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringLatin1_SimpleASCII) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_latin1(env_, "hello", 5, &result));
  ASSERT_NE(nullptr, result);

  auto content = getStringContent(result);
  EXPECT_EQ(u"hello", content);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringLatin1_AutoLength) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_create_string_latin1(env_, "world", NAPI_AUTO_LENGTH, &result));
  ASSERT_NE(nullptr, result);

  auto content = getStringContent(result);
  EXPECT_EQ(u"world", content);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringLatin1_SingleChar) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_latin1(env_, "A", 1, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(1u, str->getStringLength());
  EXPECT_EQ(u'A', str->at(0));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringLatin1_NonASCII) {
  napi_handle_scope scope = openScope(env_);

  // Latin-1 bytes for "café": c=0x63 a=0x61 f=0x66 é=0xE9
  // In Latin-1, é is byte 0xE9 which maps to U+00E9.
  const char latin1[] = "caf\xE9";
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_latin1(env_, latin1, 4, &result));
  ASSERT_NE(nullptr, result);

  auto content = getStringContent(result);
  EXPECT_EQ(u"caf\u00E9", content);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringLatin1_HighByte) {
  napi_handle_scope scope = openScope(env_);

  // Test byte 0xFF (ÿ = U+00FF, Latin small letter y with diaeresis)
  const char latin1[] = "\xFF";
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_latin1(env_, latin1, 1, &result));
  ASSERT_NE(nullptr, result);

  auto content = getStringContent(result);
  EXPECT_EQ(1u, content.size());
  EXPECT_EQ(u'\u00FF', content[0]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringLatin1_AllHighBytes) {
  napi_handle_scope scope = openScope(env_);

  // Create a string with all Latin-1 bytes from 0x80 to 0xFF.
  char latin1[128];
  char16_t expected[128];
  for (int i = 0; i < 128; ++i) {
    latin1[i] = static_cast<char>(0x80 + i);
    expected[i] = static_cast<char16_t>(0x80 + i);
  }

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_latin1(env_, latin1, 128, &result));
  ASSERT_NE(nullptr, result);

  auto content = getStringContent(result);
  EXPECT_EQ(std::u16string(expected, 128), content);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringLatin1_LongerString) {
  napi_handle_scope scope = openScope(env_);

  std::string longStr(256, 'x');
  napi_value result = nullptr;
  ASSERT_EQ(
      napi_ok,
      napi_create_string_latin1(
          env_, longStr.c_str(), longStr.size(), &result));
  ASSERT_NE(nullptr, result);

  auto content = getStringContent(result);
  EXPECT_EQ(std::u16string(256, u'x'), content);

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_string_latin1 — typeof
//===========================================================================

TEST_F(NapiTestFixture, CreateStringLatin1_TypeofIsString) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_latin1(env_, "test", 4, &result));

  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, result, &type));
  EXPECT_EQ(napi_string, type);

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_string_latin1 — GC safety
//===========================================================================

TEST_F(NapiTestFixture, CreateStringLatin1_SurvivesGC) {
  napi_handle_scope scope = openScope(env_);

  // Use a non-ASCII Latin-1 string so it's not interned.
  const char latin1[] = "caf\xE9";
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_latin1(env_, latin1, 4, &result));

  // Trigger GC — the string should survive because it's in a handle
  // scope slot which is a GC root.
  collectAndDrain();

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto content = getStringContent(result);
  EXPECT_EQ(u"caf\u00E9", content);

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_string_latin1 — argument validation
//===========================================================================

TEST_F(NapiTestFixture, CreateStringLatin1_NullEnv) {
  EXPECT_EQ(
      napi_invalid_arg,
      napi_create_string_latin1(nullptr, "hello", 5, nullptr));
}

TEST_F(NapiTestFixture, CreateStringLatin1_NullResult) {
  EXPECT_EQ(
      napi_invalid_arg, napi_create_string_latin1(env_, "hello", 5, nullptr));
}

TEST_F(NapiTestFixture, CreateStringLatin1_NullStrWithZeroLength) {
  napi_handle_scope scope = openScope(env_);

  // Null str pointer with length 0 should create an empty string.
  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_latin1(env_, nullptr, 0, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(0u, str->getStringLength());

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringLatin1_NullStrWithNonZeroLength) {
  // Null str pointer with non-zero length should fail.
  napi_value result = nullptr;
  EXPECT_EQ(
      napi_invalid_arg, napi_create_string_latin1(env_, nullptr, 5, &result));
}

TEST_F(NapiTestFixture, CreateStringLatin1_ClearsError) {
  napi_handle_scope scope = openScope(env_);

  // Set an error state first.
  napi_set_last_error(env_, napi_generic_failure);
  EXPECT_EQ(napi_generic_failure, env_->last_error.error_code);

  napi_value result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_latin1(env_, "test", 4, &result));
  EXPECT_EQ(napi_ok, env_->last_error.error_code);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringLatin1_MultipleStringsInScope) {
  napi_handle_scope scope = openScope(env_);

  napi_value s1 = nullptr;
  napi_value s2 = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_latin1(env_, "foo", 3, &s1));
  ASSERT_EQ(napi_ok, napi_create_string_latin1(env_, "bar", 3, &s2));

  // Different handle scope slots.
  EXPECT_NE(s1, s2);

  // Both should be valid strings with correct content.
  auto content1 = getStringContent(s1);
  auto content2 = getStringContent(s2);
  EXPECT_EQ(u"foo", content1);
  EXPECT_EQ(u"bar", content2);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringLatin1_EmptyWithAutoLength) {
  napi_handle_scope scope = openScope(env_);

  napi_value result = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_string_latin1(env_, "", NAPI_AUTO_LENGTH, &result));
  ASSERT_NE(nullptr, result);

  auto *phv = reinterpret_cast<PinnedHermesValue *>(result);
  EXPECT_TRUE(phv->isString());

  auto *str = vmcast<StringPrimitive>(*phv);
  EXPECT_EQ(0u, str->getStringLength());

  closeScope(env_, scope);
}

//===========================================================================
// napi_create_string_latin1 — roundtrip with UTF-16
//===========================================================================

TEST_F(NapiTestFixture, CreateStringLatin1_RoundtripWithUtf16Creation) {
  napi_handle_scope scope = openScope(env_);

  // Create "café" via Latin-1 (é = byte 0xE9)
  const char latin1[] = "caf\xE9";
  napi_value latin1Result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_latin1(env_, latin1, 4, &latin1Result));

  // Create the same string via UTF-16 (é = U+00E9)
  napi_value utf16Result = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_string_utf16(env_, u"caf\u00E9", 4, &utf16Result));

  // Both should have the same content.
  auto contentLatin1 = getStringContent(latin1Result);
  auto contentUtf16 = getStringContent(utf16Result);
  EXPECT_EQ(contentLatin1, contentUtf16);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateStringLatin1_RoundtripWithUtf8Creation) {
  napi_handle_scope scope = openScope(env_);

  // Create "café" via Latin-1 (é = byte 0xE9)
  const char latin1[] = "caf\xE9";
  napi_value latin1Result = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_latin1(env_, latin1, 4, &latin1Result));

  // Create the same string via UTF-8 (é = 0xC3 0xA9)
  napi_value utf8Result = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_string_utf8(env_, "caf\xC3\xA9", 5, &utf8Result));

  // Both should have the same content.
  auto contentLatin1 = getStringContent(latin1Result);
  auto contentUtf8 = getStringContent(utf8Result);
  EXPECT_EQ(contentLatin1, contentUtf8);

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_string_utf8 — basic extraction
//===========================================================================

TEST_F(NapiTestFixture, GetValueStringUtf8_SimpleASCII) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // First, query the required buffer size.
  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf8(env_, str, nullptr, 0, &len));
  EXPECT_EQ(5u, len);

  // Now extract the string into a buffer.
  char buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_utf8(env_, str, buf, sizeof(buf), &written));
  EXPECT_EQ(5u, written);
  EXPECT_STREQ("hello", buf);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf8_EmptyString) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "", 0, &str));

  // Query size.
  size_t len = 42;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf8(env_, str, nullptr, 0, &len));
  EXPECT_EQ(0u, len);

  // Extract into buffer.
  char buf[4] = "xxx";
  size_t written = 42;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_utf8(env_, str, buf, sizeof(buf), &written));
  EXPECT_EQ(0u, written);
  EXPECT_STREQ("", buf);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf8_MultibyteUTF8) {
  napi_handle_scope scope = openScope(env_);

  // "café" is 5 UTF-8 bytes: c(1) a(1) f(1) é(2)
  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "caf\xC3\xA9", 5, &str));

  // Query required size.
  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf8(env_, str, nullptr, 0, &len));
  EXPECT_EQ(5u, len);

  // Extract and verify.
  char buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_utf8(env_, str, buf, sizeof(buf), &written));
  EXPECT_EQ(5u, written);
  EXPECT_EQ(0, std::memcmp(buf, "caf\xC3\xA9", 5));
  EXPECT_EQ('\0', buf[5]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf8_ThreeByte) {
  napi_handle_scope scope = openScope(env_);

  // U+2603 SNOWMAN = 3 UTF-8 bytes: 0xE2 0x98 0x83
  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "\xE2\x98\x83", 3, &str));

  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf8(env_, str, nullptr, 0, &len));
  EXPECT_EQ(3u, len);

  char buf[8] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_utf8(env_, str, buf, sizeof(buf), &written));
  EXPECT_EQ(3u, written);
  EXPECT_EQ(0, std::memcmp(buf, "\xE2\x98\x83", 3));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf8_FourByteEmoji) {
  napi_handle_scope scope = openScope(env_);

  // U+1F600 GRINNING FACE = 4 UTF-8 bytes
  napi_value str = nullptr;
  ASSERT_EQ(
      napi_ok, napi_create_string_utf8(env_, "\xF0\x9F\x98\x80", 4, &str));

  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf8(env_, str, nullptr, 0, &len));
  EXPECT_EQ(4u, len);

  char buf[8] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_utf8(env_, str, buf, sizeof(buf), &written));
  EXPECT_EQ(4u, written);
  EXPECT_EQ(0, std::memcmp(buf, "\xF0\x9F\x98\x80", 4));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_string_utf8 — buffer sizing and truncation
//===========================================================================

TEST_F(NapiTestFixture, GetValueStringUtf8_TruncationASCII) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // Only provide a 4-byte buffer: should copy 3 chars + null.
  char buf[4] = {};
  size_t written = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf8(env_, str, buf, 4, &written));
  EXPECT_EQ(3u, written);
  EXPECT_STREQ("hel", buf);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf8_TruncationUTF8) {
  napi_handle_scope scope = openScope(env_);

  // "café" is c(1) a(1) f(1) é(2) = 5 UTF-8 bytes.
  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "caf\xC3\xA9", 5, &str));

  // Provide a 5-byte buffer: space for 4 bytes + null.
  // "caf" fits (3 bytes), but "é" (2 bytes) would overflow, so
  // only "caf" should be copied (3 bytes + null).
  char buf[5] = {};
  size_t written = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf8(env_, str, buf, 5, &written));
  // Should get either 3 or 4 bytes. The buffer converter writes
  // complete characters only, so "caf" (3 bytes) is expected.
  EXPECT_LE(written, 4u);
  buf[written] = '\0';

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf8_BufsizeOne) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // bufsize=1: only room for the null terminator.
  char buf[2] = "x";
  size_t written = 42;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf8(env_, str, buf, 1, &written));
  EXPECT_EQ(0u, written);
  EXPECT_STREQ("", buf);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf8_BufsizeZero) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // bufsize=0: nothing should be written.
  char buf[4] = "xxx";
  size_t written = 42;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf8(env_, str, buf, 0, &written));
  EXPECT_EQ(0u, written);
  // Buffer should be untouched.
  EXPECT_EQ('x', buf[0]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf8_ExactFit) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "abcd", 4, &str));

  // bufsize=5: exactly enough for 4 chars + null.
  char buf[5] = {};
  size_t written = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf8(env_, str, buf, 5, &written));
  EXPECT_EQ(4u, written);
  EXPECT_STREQ("abcd", buf);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf8_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // result can be null when buf is non-null.
  char buf[16] = {};
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_utf8(env_, str, buf, sizeof(buf), nullptr));
  EXPECT_STREQ("hello", buf);

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_string_utf8 — type checking
//===========================================================================

TEST_F(NapiTestFixture, GetValueStringUtf8_NotAString) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 42.0, &num));

  size_t len = 0;
  EXPECT_EQ(
      napi_string_expected,
      napi_get_value_string_utf8(env_, num, nullptr, 0, &len));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf8_Undefined) {
  napi_handle_scope scope = openScope(env_);

  napi_value undef = nullptr;
  ASSERT_EQ(napi_ok, napi_get_undefined(env_, &undef));

  char buf[8] = {};
  size_t written = 0;
  EXPECT_EQ(
      napi_string_expected,
      napi_get_value_string_utf8(env_, undef, buf, sizeof(buf), &written));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_string_utf8 — argument validation
//===========================================================================

TEST_F(NapiTestFixture, GetValueStringUtf8_NullEnv) {
  EXPECT_EQ(
      napi_invalid_arg,
      napi_get_value_string_utf8(nullptr, nullptr, nullptr, 0, nullptr));
}

TEST_F(NapiTestFixture, GetValueStringUtf8_NullValue) {
  EXPECT_EQ(
      napi_invalid_arg,
      napi_get_value_string_utf8(env_, nullptr, nullptr, 0, nullptr));
}

TEST_F(NapiTestFixture, GetValueStringUtf8_NullBufAndNullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // buf is null but result is also null — should fail because
  // in size query mode, result is required.
  EXPECT_EQ(
      napi_invalid_arg,
      napi_get_value_string_utf8(env_, str, nullptr, 0, nullptr));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_string_utf8 — roundtrip from UTF-16 creation
//===========================================================================

TEST_F(NapiTestFixture, GetValueStringUtf8_FromUTF16Creation) {
  napi_handle_scope scope = openScope(env_);

  // Create a string via UTF-16 with non-ASCII content.
  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, u"caf\u00E9", 4, &str));

  // Query UTF-8 size: "café" = c(1) a(1) f(1) é(2) = 5 bytes.
  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf8(env_, str, nullptr, 0, &len));
  EXPECT_EQ(5u, len);

  // Extract and verify.
  char buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_utf8(env_, str, buf, sizeof(buf), &written));
  EXPECT_EQ(5u, written);
  EXPECT_EQ(0, std::memcmp(buf, "caf\xC3\xA9", 5));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf8_FromLatin1Creation) {
  napi_handle_scope scope = openScope(env_);

  // Create a string via Latin-1. Latin-1 0xE9 = U+00E9 = UTF-8 0xC3 0xA9.
  const char latin1[] = "caf\xE9";
  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_latin1(env_, latin1, 4, &str));

  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf8(env_, str, nullptr, 0, &len));
  EXPECT_EQ(5u, len);

  char buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_utf8(env_, str, buf, sizeof(buf), &written));
  EXPECT_EQ(5u, written);
  EXPECT_EQ(0, std::memcmp(buf, "caf\xC3\xA9", 5));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_string_utf8 — GC safety
//===========================================================================

TEST_F(NapiTestFixture, GetValueStringUtf8_AfterGC) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // Trigger GC before extracting.
  collectAndDrain();

  char buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_utf8(env_, str, buf, sizeof(buf), &written));
  EXPECT_EQ(5u, written);
  EXPECT_STREQ("hello", buf);

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_string_utf16 — basic extraction
//===========================================================================

TEST_F(NapiTestFixture, GetValueStringUtf16_SimpleASCII) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // Query the required buffer size.
  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, nullptr, 0, &len));
  EXPECT_EQ(5u, len);

  // Extract the string into a buffer.
  char16_t buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, buf, 16, &written));
  EXPECT_EQ(5u, written);
  EXPECT_EQ(u"hello", std::u16string(buf, written));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf16_EmptyString) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "", 0, &str));

  // Query size.
  size_t len = 42;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, nullptr, 0, &len));
  EXPECT_EQ(0u, len);

  // Extract into buffer.
  char16_t buf[4] = {u'x', u'x', u'x', u'\0'};
  size_t written = 42;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, buf, 4, &written));
  EXPECT_EQ(0u, written);
  EXPECT_EQ(u'\0', buf[0]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf16_NonASCII) {
  napi_handle_scope scope = openScope(env_);

  // "café" via UTF-16 creation — stored natively as UTF-16.
  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, u"caf\u00E9", 4, &str));

  // Query size.
  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, nullptr, 0, &len));
  EXPECT_EQ(4u, len);

  // Extract and verify.
  char16_t buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, buf, 16, &written));
  EXPECT_EQ(4u, written);
  EXPECT_EQ(u"caf\u00E9", std::u16string(buf, written));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf16_SurrogatePair) {
  napi_handle_scope scope = openScope(env_);

  // U+1F600 GRINNING FACE as surrogate pair: 0xD83D 0xDE00
  const char16_t emoji[] = {0xD83D, 0xDE00, 0};
  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, emoji, 2, &str));

  // Query size: 2 UTF-16 code units.
  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, nullptr, 0, &len));
  EXPECT_EQ(2u, len);

  // Extract and verify.
  char16_t buf[8] = {};
  size_t written = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, buf, 8, &written));
  EXPECT_EQ(2u, written);
  EXPECT_EQ(u'\xD83D', buf[0]);
  EXPECT_EQ(u'\xDE00', buf[1]);
  EXPECT_EQ(u'\0', buf[2]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf16_BMP) {
  napi_handle_scope scope = openScope(env_);

  // U+2603 SNOWMAN
  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, u"\u2603", 1, &str));

  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, nullptr, 0, &len));
  EXPECT_EQ(1u, len);

  char16_t buf[4] = {};
  size_t written = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, buf, 4, &written));
  EXPECT_EQ(1u, written);
  EXPECT_EQ(u'\u2603', buf[0]);

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_string_utf16 — buffer sizing and truncation
//===========================================================================

TEST_F(NapiTestFixture, GetValueStringUtf16_TruncationASCII) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // Provide a 4-element buffer: should copy 3 code units + null.
  char16_t buf[4] = {};
  size_t written = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, buf, 4, &written));
  EXPECT_EQ(3u, written);
  EXPECT_EQ(u"hel", std::u16string(buf, written));
  EXPECT_EQ(u'\0', buf[3]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf16_TruncationUTF16) {
  napi_handle_scope scope = openScope(env_);

  // "café" = 4 UTF-16 code units.
  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, u"caf\u00E9", 4, &str));

  // Buffer of 4: space for 3 code units + null.
  char16_t buf[4] = {};
  size_t written = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, buf, 4, &written));
  EXPECT_EQ(3u, written);
  EXPECT_EQ(u"caf", std::u16string(buf, written));
  EXPECT_EQ(u'\0', buf[3]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf16_BufsizeOne) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // bufsize=1: only room for the null terminator.
  char16_t buf[2] = {u'x', u'\0'};
  size_t written = 42;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, buf, 1, &written));
  EXPECT_EQ(0u, written);
  EXPECT_EQ(u'\0', buf[0]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf16_BufsizeZero) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // bufsize=0: nothing should be written.
  char16_t buf[4] = {u'x', u'x', u'x', u'\0'};
  size_t written = 42;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, buf, 0, &written));
  EXPECT_EQ(0u, written);
  // Buffer should be untouched.
  EXPECT_EQ(u'x', buf[0]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf16_ExactFit) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "abcd", 4, &str));

  // bufsize=5: exactly enough for 4 code units + null.
  char16_t buf[5] = {};
  size_t written = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, buf, 5, &written));
  EXPECT_EQ(4u, written);
  EXPECT_EQ(u"abcd", std::u16string(buf, written));
  EXPECT_EQ(u'\0', buf[4]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf16_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // result can be null when buf is non-null.
  char16_t buf[16] = {};
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, buf, 16, nullptr));
  EXPECT_EQ(u"hello", std::u16string(buf));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_string_utf16 — type checking
//===========================================================================

TEST_F(NapiTestFixture, GetValueStringUtf16_NotAString) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 42.0, &num));

  size_t len = 0;
  EXPECT_EQ(
      napi_string_expected,
      napi_get_value_string_utf16(env_, num, nullptr, 0, &len));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf16_Undefined) {
  napi_handle_scope scope = openScope(env_);

  napi_value undef = nullptr;
  ASSERT_EQ(napi_ok, napi_get_undefined(env_, &undef));

  char16_t buf[8] = {};
  size_t written = 0;
  EXPECT_EQ(
      napi_string_expected,
      napi_get_value_string_utf16(env_, undef, buf, 8, &written));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_string_utf16 — argument validation
//===========================================================================

TEST_F(NapiTestFixture, GetValueStringUtf16_NullEnv) {
  EXPECT_EQ(
      napi_invalid_arg,
      napi_get_value_string_utf16(nullptr, nullptr, nullptr, 0, nullptr));
}

TEST_F(NapiTestFixture, GetValueStringUtf16_NullValue) {
  EXPECT_EQ(
      napi_invalid_arg,
      napi_get_value_string_utf16(env_, nullptr, nullptr, 0, nullptr));
}

TEST_F(NapiTestFixture, GetValueStringUtf16_NullBufAndNullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // buf is null but result is also null — should fail because
  // in size query mode, result is required.
  EXPECT_EQ(
      napi_invalid_arg,
      napi_get_value_string_utf16(env_, str, nullptr, 0, nullptr));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_string_utf16 — roundtrip from various creation methods
//===========================================================================

TEST_F(NapiTestFixture, GetValueStringUtf16_FromUTF8Creation) {
  napi_handle_scope scope = openScope(env_);

  // Create a string via UTF-8 with non-ASCII content.
  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "caf\xC3\xA9", 5, &str));

  // Query UTF-16 size: "café" = 4 code units.
  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, nullptr, 0, &len));
  EXPECT_EQ(4u, len);

  // Extract and verify.
  char16_t buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, buf, 16, &written));
  EXPECT_EQ(4u, written);
  EXPECT_EQ(u"caf\u00E9", std::u16string(buf, written));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf16_FromLatin1Creation) {
  napi_handle_scope scope = openScope(env_);

  // Create a string via Latin-1. Latin-1 0xE9 = U+00E9.
  const char latin1[] = "caf\xE9";
  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_latin1(env_, latin1, 4, &str));

  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, nullptr, 0, &len));
  EXPECT_EQ(4u, len);

  char16_t buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, buf, 16, &written));
  EXPECT_EQ(4u, written);
  EXPECT_EQ(u"caf\u00E9", std::u16string(buf, written));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringUtf16_FromUTF16Creation) {
  napi_handle_scope scope = openScope(env_);

  // Create via UTF-16 and extract back — full roundtrip.
  const char16_t input[] = u"Hello \u2603 World";
  size_t inputLen = std::char_traits<char16_t>::length(input);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, input, inputLen, &str));

  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, nullptr, 0, &len));
  EXPECT_EQ(inputLen, len);

  char16_t buf[32] = {};
  size_t written = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, buf, 32, &written));
  EXPECT_EQ(inputLen, written);
  EXPECT_EQ(std::u16string(input), std::u16string(buf, written));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_string_utf16 — GC safety
//===========================================================================

TEST_F(NapiTestFixture, GetValueStringUtf16_AfterGC) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // Trigger GC before extracting.
  collectAndDrain();

  char16_t buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_utf16(env_, str, buf, 16, &written));
  EXPECT_EQ(5u, written);
  EXPECT_EQ(u"hello", std::u16string(buf, written));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_string_latin1 — basic extraction
//===========================================================================

TEST_F(NapiTestFixture, GetValueStringLatin1_SimpleASCII) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // Query the required buffer size.
  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_latin1(env_, str, nullptr, 0, &len));
  EXPECT_EQ(5u, len);

  // Extract the string into a buffer.
  char buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_latin1(env_, str, buf, sizeof(buf), &written));
  EXPECT_EQ(5u, written);
  EXPECT_STREQ("hello", buf);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringLatin1_EmptyString) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "", 0, &str));

  // Query size.
  size_t len = 42;
  ASSERT_EQ(napi_ok, napi_get_value_string_latin1(env_, str, nullptr, 0, &len));
  EXPECT_EQ(0u, len);

  // Extract into buffer.
  char buf[4] = "xxx";
  size_t written = 42;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_latin1(env_, str, buf, sizeof(buf), &written));
  EXPECT_EQ(0u, written);
  EXPECT_STREQ("", buf);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringLatin1_RoundtripLatin1) {
  napi_handle_scope scope = openScope(env_);

  // Create "café" via Latin-1 (é = byte 0xE9).
  const char latin1[] = "caf\xE9";
  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_latin1(env_, latin1, 4, &str));

  // Query size.
  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_latin1(env_, str, nullptr, 0, &len));
  EXPECT_EQ(4u, len);

  // Extract and verify roundtrip.
  char buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_latin1(env_, str, buf, sizeof(buf), &written));
  EXPECT_EQ(4u, written);
  EXPECT_EQ(0, std::memcmp(buf, "caf\xE9", 4));
  EXPECT_EQ('\0', buf[4]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringLatin1_HighBytesRoundtrip) {
  napi_handle_scope scope = openScope(env_);

  // Create a string with all bytes 0x80-0xFF via Latin-1.
  char latin1[128];
  for (int i = 0; i < 128; ++i) {
    latin1[i] = static_cast<char>(0x80 + i);
  }

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_latin1(env_, latin1, 128, &str));

  // Extract back as Latin-1.
  char buf[256] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_latin1(env_, str, buf, sizeof(buf), &written));
  EXPECT_EQ(128u, written);
  EXPECT_EQ(0, std::memcmp(buf, latin1, 128));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringLatin1_FromUTF16NonLatin1) {
  napi_handle_scope scope = openScope(env_);

  // Create a string with characters outside Latin-1 range via UTF-16.
  // U+2603 SNOWMAN (code unit 0x2603) should truncate to 0x03 in Latin-1.
  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf16(env_, u"\u2603", 1, &str));

  // Size query: 1 character.
  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_latin1(env_, str, nullptr, 0, &len));
  EXPECT_EQ(1u, len);

  // Extract: U+2603 truncated to low byte = 0x03.
  char buf[4] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_latin1(env_, str, buf, sizeof(buf), &written));
  EXPECT_EQ(1u, written);
  EXPECT_EQ('\x03', buf[0]);
  EXPECT_EQ('\0', buf[1]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringLatin1_FromUTF8NonASCII) {
  napi_handle_scope scope = openScope(env_);

  // Create "café" via UTF-8 (é = 0xC3 0xA9 = U+00E9).
  // In Latin-1, U+00E9 = byte 0xE9.
  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "caf\xC3\xA9", 5, &str));

  size_t len = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_latin1(env_, str, nullptr, 0, &len));
  EXPECT_EQ(4u, len);

  char buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_latin1(env_, str, buf, sizeof(buf), &written));
  EXPECT_EQ(4u, written);
  EXPECT_EQ(0, std::memcmp(buf, "caf\xE9", 4));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_string_latin1 — buffer sizing and truncation
//===========================================================================

TEST_F(NapiTestFixture, GetValueStringLatin1_TruncationASCII) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // Only provide a 4-byte buffer: should copy 3 chars + null.
  char buf[4] = {};
  size_t written = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_latin1(env_, str, buf, 4, &written));
  EXPECT_EQ(3u, written);
  EXPECT_STREQ("hel", buf);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringLatin1_BufsizeOne) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // bufsize=1: only room for the null terminator.
  char buf[2] = "x";
  size_t written = 42;
  ASSERT_EQ(napi_ok, napi_get_value_string_latin1(env_, str, buf, 1, &written));
  EXPECT_EQ(0u, written);
  EXPECT_STREQ("", buf);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringLatin1_BufsizeZero) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // bufsize=0: nothing should be written.
  char buf[4] = "xxx";
  size_t written = 42;
  ASSERT_EQ(napi_ok, napi_get_value_string_latin1(env_, str, buf, 0, &written));
  EXPECT_EQ(0u, written);
  // Buffer should be untouched.
  EXPECT_EQ('x', buf[0]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringLatin1_ExactFit) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "abcd", 4, &str));

  // bufsize=5: exactly enough for 4 chars + null.
  char buf[5] = {};
  size_t written = 0;
  ASSERT_EQ(napi_ok, napi_get_value_string_latin1(env_, str, buf, 5, &written));
  EXPECT_EQ(4u, written);
  EXPECT_STREQ("abcd", buf);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringLatin1_NullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // result can be null when buf is non-null.
  char buf[16] = {};
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_latin1(env_, str, buf, sizeof(buf), nullptr));
  EXPECT_STREQ("hello", buf);

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_string_latin1 — type checking
//===========================================================================

TEST_F(NapiTestFixture, GetValueStringLatin1_NotAString) {
  napi_handle_scope scope = openScope(env_);

  napi_value num = nullptr;
  ASSERT_EQ(napi_ok, napi_create_double(env_, 42.0, &num));

  size_t len = 0;
  EXPECT_EQ(
      napi_string_expected,
      napi_get_value_string_latin1(env_, num, nullptr, 0, &len));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, GetValueStringLatin1_Undefined) {
  napi_handle_scope scope = openScope(env_);

  napi_value undef = nullptr;
  ASSERT_EQ(napi_ok, napi_get_undefined(env_, &undef));

  char buf[8] = {};
  size_t written = 0;
  EXPECT_EQ(
      napi_string_expected,
      napi_get_value_string_latin1(env_, undef, buf, sizeof(buf), &written));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_string_latin1 — argument validation
//===========================================================================

TEST_F(NapiTestFixture, GetValueStringLatin1_NullEnv) {
  EXPECT_EQ(
      napi_invalid_arg,
      napi_get_value_string_latin1(nullptr, nullptr, nullptr, 0, nullptr));
}

TEST_F(NapiTestFixture, GetValueStringLatin1_NullValue) {
  EXPECT_EQ(
      napi_invalid_arg,
      napi_get_value_string_latin1(env_, nullptr, nullptr, 0, nullptr));
}

TEST_F(NapiTestFixture, GetValueStringLatin1_NullBufAndNullResult) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // buf is null but result is also null — should fail because
  // in size query mode, result is required.
  EXPECT_EQ(
      napi_invalid_arg,
      napi_get_value_string_latin1(env_, str, nullptr, 0, nullptr));

  closeScope(env_, scope);
}

//===========================================================================
// napi_get_value_string_latin1 — GC safety
//===========================================================================

TEST_F(NapiTestFixture, GetValueStringLatin1_AfterGC) {
  napi_handle_scope scope = openScope(env_);

  napi_value str = nullptr;
  ASSERT_EQ(napi_ok, napi_create_string_utf8(env_, "hello", 5, &str));

  // Trigger GC before extracting.
  collectAndDrain();

  char buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_latin1(env_, str, buf, sizeof(buf), &written));
  EXPECT_EQ(5u, written);
  EXPECT_STREQ("hello", buf);

  closeScope(env_, scope);
}

//===========================================================================
// node_api_create_external_string_latin1
//===========================================================================

TEST_F(NapiTestFixture, CreateExternalStringLatin1_NullEnv) {
  char str[] = "hello";
  napi_value result = nullptr;
  EXPECT_EQ(
      napi_invalid_arg,
      node_api_create_external_string_latin1(
          nullptr, str, 5, nullptr, nullptr, &result, nullptr));
}

TEST_F(NapiTestFixture, CreateExternalStringLatin1_NullStr) {
  napi_handle_scope scope = openScope(env_);
  napi_value result = nullptr;
  EXPECT_EQ(
      napi_invalid_arg,
      node_api_create_external_string_latin1(
          env_, nullptr, 5, nullptr, nullptr, &result, nullptr));
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateExternalStringLatin1_NullResult) {
  char str[] = "hello";
  EXPECT_EQ(
      napi_invalid_arg,
      node_api_create_external_string_latin1(
          env_, str, 5, nullptr, nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, CreateExternalStringLatin1_Basic) {
  napi_handle_scope scope = openScope(env_);

  char str[] = "hello";
  napi_value result = nullptr;
  bool copied = false;
  ASSERT_EQ(
      napi_ok,
      node_api_create_external_string_latin1(
          env_, str, 5, nullptr, nullptr, &result, &copied));

  // Hermes copies the data, so copied should be true.
  EXPECT_TRUE(copied);

  // Verify the string content matches.
  char buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_latin1(env_, result, buf, sizeof(buf), &written));
  EXPECT_EQ(5u, written);
  EXPECT_STREQ("hello", buf);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateExternalStringLatin1_AutoLength) {
  napi_handle_scope scope = openScope(env_);

  char str[] = "auto length";
  napi_value result = nullptr;
  bool copied = false;
  ASSERT_EQ(
      napi_ok,
      node_api_create_external_string_latin1(
          env_, str, NAPI_AUTO_LENGTH, nullptr, nullptr, &result, &copied));

  EXPECT_TRUE(copied);

  char buf[32] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_latin1(env_, result, buf, sizeof(buf), &written));
  EXPECT_EQ(11u, written);
  EXPECT_STREQ("auto length", buf);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateExternalStringLatin1_FinalizerCalled) {
  napi_handle_scope scope = openScope(env_);

  bool finalizerCalled = false;
  char str[] = "test";
  napi_value result = nullptr;

  auto finalizer = [](napi_env, void *data, void *hint) {
    *static_cast<bool *>(hint) = true;
  };

  ASSERT_EQ(
      napi_ok,
      node_api_create_external_string_latin1(
          env_, str, 4, finalizer, &finalizerCalled, &result, nullptr));

  // Since Hermes copies, the finalizer should be called immediately.
  EXPECT_TRUE(finalizerCalled);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateExternalStringLatin1_NullCopied) {
  napi_handle_scope scope = openScope(env_);

  char str[] = "test";
  napi_value result = nullptr;
  // Pass nullptr for copied — should not crash.
  ASSERT_EQ(
      napi_ok,
      node_api_create_external_string_latin1(
          env_, str, 4, nullptr, nullptr, &result, nullptr));

  char buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_latin1(env_, result, buf, sizeof(buf), &written));
  EXPECT_EQ(4u, written);
  EXPECT_STREQ("test", buf);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateExternalStringLatin1_HighBytes) {
  napi_handle_scope scope = openScope(env_);

  // Latin-1 chars with code points > 127.
  char str[] = "\xC0\xE9\xFF";
  napi_value result = nullptr;
  bool copied = false;
  ASSERT_EQ(
      napi_ok,
      node_api_create_external_string_latin1(
          env_, str, 3, nullptr, nullptr, &result, &copied));

  EXPECT_TRUE(copied);

  char buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok,
      napi_get_value_string_latin1(env_, result, buf, sizeof(buf), &written));
  EXPECT_EQ(3u, written);
  EXPECT_EQ('\xC0', buf[0]);
  EXPECT_EQ('\xE9', buf[1]);
  EXPECT_EQ('\xFF', buf[2]);

  closeScope(env_, scope);
}

//===========================================================================
// node_api_create_external_string_utf16
//===========================================================================

TEST_F(NapiTestFixture, CreateExternalStringUtf16_NullEnv) {
  char16_t str[] = u"hello";
  napi_value result = nullptr;
  EXPECT_EQ(
      napi_invalid_arg,
      node_api_create_external_string_utf16(
          nullptr, str, 5, nullptr, nullptr, &result, nullptr));
}

TEST_F(NapiTestFixture, CreateExternalStringUtf16_NullStr) {
  napi_handle_scope scope = openScope(env_);
  napi_value result = nullptr;
  EXPECT_EQ(
      napi_invalid_arg,
      node_api_create_external_string_utf16(
          env_, nullptr, 5, nullptr, nullptr, &result, nullptr));
  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateExternalStringUtf16_NullResult) {
  char16_t str[] = u"hello";
  EXPECT_EQ(
      napi_invalid_arg,
      node_api_create_external_string_utf16(
          env_, str, 5, nullptr, nullptr, nullptr, nullptr));
}

TEST_F(NapiTestFixture, CreateExternalStringUtf16_Basic) {
  napi_handle_scope scope = openScope(env_);

  char16_t str[] = u"hello";
  napi_value result = nullptr;
  bool copied = false;
  ASSERT_EQ(
      napi_ok,
      node_api_create_external_string_utf16(
          env_, str, 5, nullptr, nullptr, &result, &copied));

  // Hermes copies the data, so copied should be true.
  EXPECT_TRUE(copied);

  // Verify the string content matches.
  char16_t buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok, napi_get_value_string_utf16(env_, result, buf, 16, &written));
  EXPECT_EQ(5u, written);
  EXPECT_EQ(0, std::char_traits<char16_t>::compare(u"hello", buf, 5));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateExternalStringUtf16_AutoLength) {
  napi_handle_scope scope = openScope(env_);

  char16_t str[] = u"auto length";
  napi_value result = nullptr;
  bool copied = false;
  ASSERT_EQ(
      napi_ok,
      node_api_create_external_string_utf16(
          env_, str, NAPI_AUTO_LENGTH, nullptr, nullptr, &result, &copied));

  EXPECT_TRUE(copied);

  char16_t buf[32] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok, napi_get_value_string_utf16(env_, result, buf, 32, &written));
  EXPECT_EQ(11u, written);
  EXPECT_EQ(0, std::char_traits<char16_t>::compare(u"auto length", buf, 11));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateExternalStringUtf16_FinalizerCalled) {
  napi_handle_scope scope = openScope(env_);

  bool finalizerCalled = false;
  char16_t str[] = u"test";
  napi_value result = nullptr;

  auto finalizer = [](napi_env, void *data, void *hint) {
    *static_cast<bool *>(hint) = true;
  };

  ASSERT_EQ(
      napi_ok,
      node_api_create_external_string_utf16(
          env_, str, 4, finalizer, &finalizerCalled, &result, nullptr));

  // Since Hermes copies, the finalizer should be called immediately.
  EXPECT_TRUE(finalizerCalled);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateExternalStringUtf16_NullCopied) {
  napi_handle_scope scope = openScope(env_);

  char16_t str[] = u"test";
  napi_value result = nullptr;
  // Pass nullptr for copied — should not crash.
  ASSERT_EQ(
      napi_ok,
      node_api_create_external_string_utf16(
          env_, str, 4, nullptr, nullptr, &result, nullptr));

  char16_t buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok, napi_get_value_string_utf16(env_, result, buf, 16, &written));
  EXPECT_EQ(4u, written);
  EXPECT_EQ(0, std::char_traits<char16_t>::compare(u"test", buf, 4));

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateExternalStringUtf16_NonASCII) {
  napi_handle_scope scope = openScope(env_);

  // String with non-ASCII characters.
  char16_t str[] = u"\u00E9\u00F1\u00FC"; // é ñ ü
  napi_value result = nullptr;
  bool copied = false;
  ASSERT_EQ(
      napi_ok,
      node_api_create_external_string_utf16(
          env_, str, 3, nullptr, nullptr, &result, &copied));

  EXPECT_TRUE(copied);

  char16_t buf[16] = {};
  size_t written = 0;
  ASSERT_EQ(
      napi_ok, napi_get_value_string_utf16(env_, result, buf, 16, &written));
  EXPECT_EQ(3u, written);
  EXPECT_EQ(u'\u00E9', buf[0]);
  EXPECT_EQ(u'\u00F1', buf[1]);
  EXPECT_EQ(u'\u00FC', buf[2]);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateExternalStringLatin1_EmptyString) {
  napi_handle_scope scope = openScope(env_);

  char str[] = "";
  napi_value result = nullptr;
  bool copied = false;
  ASSERT_EQ(
      napi_ok,
      node_api_create_external_string_latin1(
          env_, str, 0, nullptr, nullptr, &result, &copied));

  EXPECT_TRUE(copied);

  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, result, &type));
  EXPECT_EQ(napi_string, type);

  closeScope(env_, scope);
}

TEST_F(NapiTestFixture, CreateExternalStringUtf16_EmptyString) {
  napi_handle_scope scope = openScope(env_);

  char16_t str[] = u"";
  napi_value result = nullptr;
  bool copied = false;
  ASSERT_EQ(
      napi_ok,
      node_api_create_external_string_utf16(
          env_, str, 0, nullptr, nullptr, &result, &copied));

  EXPECT_TRUE(copied);

  napi_valuetype type;
  ASSERT_EQ(napi_ok, napi_typeof(env_, result, &type));
  EXPECT_EQ(napi_string, type);

  closeScope(env_, scope);
}

} // namespace

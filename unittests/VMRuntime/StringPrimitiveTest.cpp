/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringView.h"

#include "llvh/Support/AlignOf.h"

#include "VMRuntimeTestHelpers.h"

#include <climits>
#include <random>

#include "gtest/gtest.h"

using namespace hermes::vm;

namespace {

using StringPrimTest = RuntimeTestFixture;

TEST_F(StringPrimTest, CreateTest) {
  auto str1 = StringPrimitive::createNoThrow(runtime, "hello");
  auto str2 = StringPrimitive::createNoThrow(runtime, "foo");

  EXPECT_EQ(5u, str1->getStringLength());
  EXPECT_TRUE(
      StringPrimitive::createStringView(runtime, str1)
          .equals(createUTF16Ref(u"hello")));

  EXPECT_EQ(3u, str2->getStringLength());
  EXPECT_TRUE(
      StringPrimitive::createStringView(runtime, str2)
          .equals(createUTF16Ref(u"foo")));
}

TEST_F(StringPrimTest, EqualityTest) {
  auto s1 = StringPrimitive::createNoThrow(runtime, createUTF16Ref(u"abc"));
  auto s2 = StringPrimitive::createNoThrow(runtime, createUTF16Ref(u"abc"));
  auto s3 = StringPrimitive::createNoThrow(runtime, createUTF16Ref(u"abcdef"));
  EXPECT_TRUE(s1->equals(s2.get()));
  EXPECT_FALSE(s1->equals(s3.get()));
}

TEST_F(StringPrimTest, CreateASCIITest) {
  auto s1 = StringPrimitive::createNoThrow(runtime, createUTF16Ref(u"hello"));
  auto s2 = StringPrimitive::createNoThrow(runtime, "hello");
  EXPECT_TRUE(s1->equals(s2.get()));
}

TEST_F(StringPrimTest, CreateEfficientTest) {
  auto handlefy = [&](CallResult<HermesValue> cr) {
    return Handle<StringPrimitive>::vmcast(runtime, *cr);
  };

  static_assert(
      StringPrimitive::EXTERNAL_STRING_MIN_SIZE > 3,
      "Test strings should be shorter than EXTERNAL_STRING_MIN_SIZE");
  std::string narrowShort = "foo";
  std::u16string wideShort(u"foo");
  auto s1 = handlefy(
      StringPrimitive::createEfficient(
          runtime, createASCIIRef(narrowShort.c_str())));
  auto s2 = handlefy(
      StringPrimitive::createEfficient(
          runtime, createUTF16Ref(wideShort.c_str())));
  auto s3 = handlefy(
      StringPrimitive::createEfficient(runtime, std::move(narrowShort)));
  auto s4 =
      handlefy(StringPrimitive::createEfficient(runtime, std::move(wideShort)));
  for (auto s : {s1, s2, s3, s4}) {
    EXPECT_TRUE(s->equals(s1.get()));
    EXPECT_EQ(3, s->getStringLength());
    EXPECT_FALSE(s->isExternal());
  }

  size_t longLength = StringPrimitive::EXTERNAL_STRING_MIN_SIZE;
  std::string narrowLong(longLength, '!');
  std::u16string wideLong(longLength, u'!');
  auto e1 = handlefy(
      StringPrimitive::createEfficient(
          runtime, createASCIIRef(narrowLong.c_str())));
  auto e2 = handlefy(
      StringPrimitive::createEfficient(
          runtime, createUTF16Ref(wideLong.c_str())));
  auto e3 = handlefy(
      StringPrimitive::createEfficient(runtime, std::move(narrowLong)));
  auto e4 =
      handlefy(StringPrimitive::createEfficient(runtime, std::move(wideLong)));
  for (auto s : {e1, e2, e3, e4}) {
    EXPECT_TRUE(s->equals(e1.get()));
    EXPECT_EQ(longLength, s->getStringLength());
    if (s == e3 || s == e4) {
      EXPECT_TRUE(s->isExternal());
    } else {
      EXPECT_FALSE(s->isExternal());
    }
  }
}

TEST_F(StringPrimTest, CompareTest) {
#define TEST_CMP(v, a, b)                                 \
  {                                                       \
    auto s1 = StringPrimitive::createNoThrow(runtime, a); \
    auto s2 = StringPrimitive::createNoThrow(runtime, b); \
    EXPECT_EQ(v, s1->compare(s2.get()));                  \
  }

  TEST_CMP(0, "", "");
  TEST_CMP(-1, "", "a");
  TEST_CMP(+1, "a", "");

  TEST_CMP(0, "123", "123");
  TEST_CMP(-1, "12", "123");
  TEST_CMP(+1, "123", "12");

  TEST_CMP(-1, "123", "4");
  TEST_CMP(+1, "4", "123");

  TEST_CMP(-1, "123", "456");
  TEST_CMP(+1, "456", "123");

#undef TEST_CMP
}

TEST_F(StringPrimTest, ConcatTest) {
  {
    auto a = StringPrimitive::createNoThrow(runtime, createUTF16Ref(u"abc"));
    auto b = StringPrimitive::createNoThrow(runtime, createUTF16Ref(u"def"));
    auto strRes = StringPrimitive::concat(runtime, a, b);
    ASSERT_NE(ExecutionStatus::EXCEPTION, strRes.getStatus());
    EXPECT_TRUE(
        StringPrimitive::createStringView(
            runtime, runtime.makeHandle<StringPrimitive>(*strRes))
            .equals(createUTF16Ref(u"abcdef")));
  }

  {
    auto a = StringPrimitive::createNoThrow(runtime, createUTF16Ref(u""));
    auto b = StringPrimitive::createNoThrow(runtime, createUTF16Ref(u"abc"));
    auto strRes = StringPrimitive::concat(runtime, a, b);
    ASSERT_NE(ExecutionStatus::EXCEPTION, strRes.getStatus());
    EXPECT_TRUE(
        StringPrimitive::createStringView(
            runtime, runtime.makeHandle<StringPrimitive>(*strRes))
            .equals(createUTF16Ref(u"abc")));
  }
}

// This attempts to test that strings above a sufficient length may be freely
// memcpy'd around. This would not be true if the small-string optimization used
// an interior pointer, or if someone else maintained a pointer to the string.
template <typename StringType>
void test1StringMemcpySafety(const StringType &s) {
  // memcpy the string so we can freely poke at its contents.
  llvh::AlignedCharArrayUnion<StringType> mb;
  memcpy(mb.buffer, &s, sizeof(StringType));

  // Grub through the string's bits, looking for pointers to the interior of the
  // string.
  intptr_t start = reinterpret_cast<intptr_t>(&s);
  intptr_t end = reinterpret_cast<intptr_t>(&s + 1);
  const intptr_t *stringGuts = reinterpret_cast<intptr_t *>(mb.buffer);
  for (size_t i = 0; i < sizeof(StringType) / sizeof(void *); i++) {
    intptr_t p = stringGuts[i];
    EXPECT_FALSE(start <= p && p <= end) << "string size: " << s.size();
    // Mask off low bits in case it uses them as flags.
    p &= ~(intptr_t)1;
    EXPECT_FALSE(start <= p && p <= end) << "string size: " << s.size();
    p &= ~(intptr_t)2;
    EXPECT_FALSE(start <= p && p <= end) << "string size: " << s.size();
  }

  // Verify the buffer, when interpreted as a string, is equal to the original.
  const StringType *memcpydStr =
      reinterpret_cast<const StringType *>(mb.buffer);
  EXPECT_EQ(s, *memcpydStr) << "string size: " << s.size();
  EXPECT_EQ(s.size(), memcpydStr->size()) << "string size: " << s.size();
  EXPECT_TRUE(std::equal(s.begin(), s.end(), memcpydStr->begin()))
      << "string size: " << s.size();

  // std::move a string. If any bits changed, it means the string is not memcpy
  // safe, because the move constructor does something nontrivial. Note it is
  // not the case in general that a moved-to string will be bitwise identical to
  // its source; the string may have an internal buffer whose last bytes are
  // unused when the string is externally allocated, and these last bytes may
  // not be copied by the move ctor. That is, the move ctor may be conceptually
  // a memcpy of only a prefix of the string. To avoid this, construct the
  // strings in buffers that are initially zeroed out. This isn't guaranteed to
  // always work because the constructors and the new operator may still modify
  // those bytes, however, it seems to work in practice.
  auto s1 = new (calloc(1, sizeof(StringType))) StringType(s);
  llvh::AlignedCharArrayUnion<StringType> savedBits;
  memcpy(savedBits.buffer, s1, sizeof(StringType));

  auto movedString =
      new (calloc(1, sizeof(StringType))) StringType(std::move(*s1));
  // We expect the move-constructed string to be bitwise identical to the string
  // before it was moved from. That is, the move ctor should be a memcpy.
  EXPECT_EQ(0, memcmp(movedString, savedBits.buffer, sizeof(StringType)))
      << "string size: " << s.size();
  // Sanity check: the moved-from string should have changed, e.g. zeroed
  // pointer.
  EXPECT_NE(0, memcmp(s1, savedBits.buffer, sizeof(StringType)))
      << "string size: " << s.size();
  movedString->~StringType();
  free(movedString);
  s1->~StringType();
  free(s1);
}

TEST_F(StringPrimTest, StringsAreMemcpySafe) {
  for (size_t length = StringPrimitive::EXTERNAL_STRING_MIN_SIZE; length < 2048;
       length++) {
    test1StringMemcpySafety(std::string(length, '!'));
    test1StringMemcpySafety(std::u16string(length, u'!'));
  }
}

/// Containers (including strings) are not memcpy-safe in Microsoft STL in debug
/// mode, because they contain a pointer to a "container proxy", which in turn
/// points back to the container. memcpy() of the container leaves the proxy
/// pointing to garbage.
/// We statically detect and work around this in our CopyableBasicString. The
/// purpose of this test is to crash if we detected wrong.
TEST_F(StringPrimTest, StringsAreMemcpySafeMicrosoftSTL) {
  using Str = CopyableBasicString<char>;
  Str *orig = new (::malloc(sizeof(Str))) Str(100, 'a');
  Str *copy = (Str *)::malloc(sizeof(Str));
  ::memcpy((void *)copy, orig, sizeof(Str));
  ::memset((void *)orig, 0xAA, sizeof(Str));
  ::free(orig);

  ASSERT_EQ(100, copy->size());
  // This should crash.
  copy->append(&copy->data()[0], &copy->data()[50]);

  copy->~Str();
  ::free(copy);
}

TEST_F(StringPrimTest, FastConcatTest) {
  CallResult<HermesValue> cr{ExecutionStatus::EXCEPTION};
  std::string bigStrA(300, 'a');
  std::string strB("small");

  //=======================================
  // ASCII + ASCII
  auto a = StringPrimitive::createNoThrow(runtime, bigStrA);
  auto b = StringPrimitive::createNoThrow(runtime, strB);
  cr = StringPrimitive::concat(runtime, a, b);
  ASSERT_NE(ExecutionStatus::EXCEPTION, cr);
  auto resASCII_1 = runtime.makeHandle<BufferedASCIIStringPrimitive>(*cr);

  std::string asciiStr1 = bigStrA + strB;
  auto asciiRef = resASCII_1->getStringRef<char>();
  EXPECT_TRUE(asciiRef.size() == asciiStr1.size());
  EXPECT_TRUE(std::equal(asciiStr1.begin(), asciiStr1.end(), asciiRef.begin()));

  //=======================================
  // Add more ASCII
  cr = StringPrimitive::concat(runtime, resASCII_1, b);
  ASSERT_NE(ExecutionStatus::EXCEPTION, cr);
  auto resASCII_2 = runtime.makeHandle<BufferedASCIIStringPrimitive>(*cr);

  // The same buffer must have been reused.
  EXPECT_TRUE(
      resASCII_1->testGetConcatBuffer() == resASCII_2->testGetConcatBuffer());

  std::string asciiStr2 = asciiStr1 + strB;
  asciiRef = resASCII_2->getStringRef<char>();
  EXPECT_TRUE(asciiRef.size() == asciiStr2.size());
  EXPECT_TRUE(std::equal(asciiStr2.begin(), asciiStr2.end(), asciiRef.begin()));

  //=======================================
  // Append some the first result again.
  cr = StringPrimitive::concat(runtime, resASCII_1, b);
  ASSERT_NE(ExecutionStatus::EXCEPTION, cr);
  auto resASCII_3 = runtime.makeHandle<BufferedASCIIStringPrimitive>(*cr);

  // The buffer cannot be reused.
  EXPECT_TRUE(
      resASCII_3->testGetConcatBuffer() != resASCII_1->testGetConcatBuffer());

  std::string asciiStr3 = asciiStr1 + strB;
  asciiRef = resASCII_3->getStringRef<char>();
  EXPECT_TRUE(asciiRef.size() == asciiStr3.size());
  EXPECT_TRUE(std::equal(asciiStr3.begin(), asciiStr3.end(), asciiRef.begin()));

  //=======================================
  // ASCII + UTF16
  std::u16string strC(u"utf16\u1234");
  b = StringPrimitive::createNoThrow(
      runtime, UTF16Ref(strC.data(), strC.size()));
  cr = StringPrimitive::concat(runtime, resASCII_2, b);
  ASSERT_NE(ExecutionStatus::EXCEPTION, cr);
  auto resUTF_1 = runtime.makeHandle<BufferedUTF16StringPrimitive>(*cr);

  std::u16string utfStr1;
  utfStr1.append(asciiStr2.begin(), asciiStr2.end());
  utfStr1.append(strC);
  auto utf16Ref = resUTF_1->getStringRef<char16_t>();
  EXPECT_TRUE(utf16Ref.size() == utfStr1.size());
  EXPECT_TRUE(std::equal(utfStr1.begin(), utfStr1.end(), utf16Ref.begin()));

  //=======================================
  // UTF16 + ASCII
  cr = StringPrimitive::concat(runtime, resUTF_1, b);
  ASSERT_NE(ExecutionStatus::EXCEPTION, cr);
  auto resUTF_2 = runtime.makeHandle<BufferedUTF16StringPrimitive>(*cr);

  // The same buffer must have been reused.
  EXPECT_TRUE(
      resUTF_1->testGetConcatBuffer() == resUTF_2->testGetConcatBuffer());

  std::u16string utfStr2 = utfStr1 + strC;
  utf16Ref = resUTF_2->getStringRef<char16_t>();
  EXPECT_TRUE(utf16Ref.size() == utfStr2.size());
  EXPECT_TRUE(std::equal(utfStr2.begin(), utfStr2.end(), utf16Ref.begin()));

  //=======================================
  // Add some more UTF16 to resUTF_1
  cr = StringPrimitive::concat(runtime, resUTF_1, b);
  ASSERT_NE(ExecutionStatus::EXCEPTION, cr);
  auto resUTF_3 = runtime.makeHandle<BufferedUTF16StringPrimitive>(*cr);

  // The buffer cannot be reused.
  EXPECT_TRUE(
      resUTF_1->testGetConcatBuffer() != resUTF_3->testGetConcatBuffer());

  std::u16string utfStr3 = utfStr1 + strC;
  utf16Ref = resUTF_3->getStringRef<char16_t>();
  EXPECT_TRUE(utf16Ref.size() == utfStr3.size());
  EXPECT_TRUE(std::equal(utfStr3.begin(), utfStr3.end(), utf16Ref.begin()));
}

/// Run \p code, which must evaluate to a string, and store it in \p out.
static void evalToString(
    Runtime &runtime,
    const char *code,
    PinnedValue<StringPrimitive> &out) {
  hermes::hbc::CompileFlags flags;
  auto res = runtime.run(code, "", flags);
  ASSERT_NE(ExecutionStatus::EXCEPTION, res);
  ASSERT_TRUE(res->isString());
  out.castAndSetHermesValue<StringPrimitive>(*res);
}

TEST_F(StringPrimTest, BuiltinsKeepASCII) {
  struct : public Locals {
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Each builtin that assembles its result in a StringBuilder must produce an
  // ASCII string when every piece of the result is ASCII, and a UTF16 string
  // otherwise.
  struct Case {
    const char *code;
    const char16_t *expected;
    bool isASCII;
  };
  static const Case cases[] = {
      {"'abc'.repeat(4)", u"abcabcabcabc", true},
      {"'a\\u00e9'.repeat(3)", u"aéaéaé", false},
      {"[1, 'a', null, 'b'].join('-')", u"1-a--b", true},
      {"['a', '\\u00e9'].join()", u"a,é", false},
      {"['a', 'b'].join('\\u00e9')", u"aéb", false},
      // The separator is not used.
      {"['a'].join('\\u00e9')", u"a", true},
      // Elements that are converted by calling toString.
      {"[{toString() { return 'x'; }}, 1, 'y'].join()", u"x,1,y", true},
      {"[{toString() { return '\\u00e9'; }}, 1].join()", u"é,1", false},
      {"[1, 'a'].toLocaleString()", u"1,a", true},
      {"[1, {toLocaleString() { return '\\u00e9'; }}].toLocaleString()",
       u"1,é",
       false},
      {"new Uint8Array([1, 2, 3]).join()", u"1,2,3", true},
      {"new Uint8Array([1, 2]).join('\\u00e9')", u"1é2", false},
      {"new Uint8Array([1, 2]).toLocaleString()", u"1,2", true},
      {"'ab'.padStart(6, 'xy')", u"xyxyab", true},
      {"'ab'.padEnd(4, '\\u00e9')", u"abéé", false},
      {"'\\u00e9'.padStart(3)", u"  é", false},
      {"'ABC'.toLowerCase()", u"abc", true},
      {"'abc'.toUpperCase()", u"ABC", true},
      {"String.fromCharCode(97, 98, 99)", u"abc", true},
      {"String.fromCharCode(97, 233)", u"aé", false},
      {"String(Symbol('ab'))", u"Symbol(ab)", true},
      {"String(Symbol('\\u00e9'))", u"Symbol(é)", false},
      {"String(new Error('m'))", u"Error: m", true},
      {"String(new Error('\\u00e9'))", u"Error: é", false},
      {"Object.prototype.toString.call({[Symbol.toStringTag]: 'T'})",
       u"[object T]",
       true},
      {"Object.prototype.toString.call({[Symbol.toStringTag]: '\\u00e9'})",
       u"[object é]",
       false},
      {"var k = 'ab';"
       "Object.getOwnPropertyDescriptor({get [k]() {}}, k).get.name",
       u"get ab",
       true},
      {"var k = '\\u00e9';"
       "Object.getOwnPropertyDescriptor({get [k]() {}}, k).get.name",
       u"get é",
       false},
  };
  for (const Case &c : cases) {
    SCOPED_TRACE(c.code);
    evalToString(runtime, c.code, lv.str);
    EXPECT_EQ(c.isASCII, lv.str->isASCII());
    EXPECT_TRUE(
        StringPrimitive::createStringView(runtime, lv.str)
            .equals(createUTF16Ref(c.expected)));
  }

  // Exactly long enough to be allocated as an external string. It must also
  // fit the test heap: external memory counts against the max heap size,
  // which MallocGC enforces exactly.
  const uint32_t externalLen = StringPrimitive::EXTERNAL_STRING_THRESHOLD;
  const std::string longCode =
      "'a'.repeat(" + std::to_string(externalLen) + ")";
  evalToString(runtime, longCode.c_str(), lv.str);
  EXPECT_TRUE(lv.str->isExternal());
  EXPECT_TRUE(lv.str->isASCII());
  EXPECT_EQ(externalLen, lv.str->getStringLength());
}

TEST_F(StringPrimTest, ExternalFromUTF16KeepsASCII) {
  struct : public Locals {
    PinnedValue<StringPrimitive> str;
  } lv;
  LocalsRAII lraii(runtime, &lv);

  // Long enough to be allocated as an external string.
  const size_t len = 1 << 17;
  const std::u16string ascii(len, u'a');
  std::u16string nonASCII(len, u'a');
  nonASCII.back() = u'é';

  /// Store \p res in lv.str and check that it holds \p expected, encoded as
  /// ASCII exactly when \p isASCII.
  auto check = [&](CallResult<HermesValue> res,
                   const std::u16string &expected,
                   bool isASCII) {
    ASSERT_NE(ExecutionStatus::EXCEPTION, res);
    lv.str.castAndSetHermesValue<StringPrimitive>(*res);
    EXPECT_EQ(isASCII, lv.str->isASCII());
    EXPECT_TRUE(
        StringPrimitive::createStringView(runtime, lv.str)
            .equals(UTF16Ref(expected.data(), expected.size())));
  };

  check(
      StringPrimitive::create(runtime, UTF16Ref(ascii.data(), len)),
      ascii,
      true);
  check(
      StringPrimitive::create(runtime, UTF16Ref(nonASCII.data(), len)),
      nonASCII,
      false);
  check(
      StringPrimitive::createWithKnownEncoding(
          runtime, UTF16Ref(ascii.data(), len), true),
      ascii,
      true);
  check(
      StringPrimitive::createWithKnownEncoding(
          runtime, UTF16Ref(nonASCII.data(), len), false),
      nonASCII,
      false);

  // An oversized length is rejected before anything is read, including ones
  // that would wrap to an external or a dynamic length if narrowed to 32 bits.
  std::vector<size_t> oversized{size_t(StringPrimitive::MAX_STRING_LENGTH) + 1};
  if (sizeof(size_t) > sizeof(uint32_t)) {
    oversized.push_back((size_t(1) << 32) + len);
    oversized.push_back((size_t(1) << 32) + 2);
  }
  for (size_t badLen : oversized) {
    UTF16Ref bad(ascii.data(), badLen);
    EXPECT_EQ(
        ExecutionStatus::EXCEPTION, StringPrimitive::create(runtime, bad));
    runtime.clearThrownValue();
    EXPECT_EQ(
        ExecutionStatus::EXCEPTION,
        StringPrimitive::createWithKnownEncoding(runtime, bad, true));
    runtime.clearThrownValue();
  }

  // JSON.stringify assembles its result in a UTF16 buffer.
  evalToString(runtime, "JSON.stringify('a'.repeat(1 << 17))", lv.str);
  EXPECT_TRUE(lv.str->isASCII());
  EXPECT_EQ((1u << 17) + 2, lv.str->getStringLength());
}
} // namespace

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VMRuntimeTestHelpers.h"

#include "hermes/Support/UTF16Stream.h"
#include "hermes/VM/JSLib/RuntimeJSONParse.h"

#include <cstdint>
#include <string>

using namespace hermes::vm;

namespace {

class RuntimeJSONUtilsTest : public RuntimeTestFixture {
 public:
  SymbolID symbolFor(std::string s) {
    return **runtime.getIdentifierTable().getSymbolHandle(
        runtime, createASCIIRef(s.data()));
  }
};

TEST_F(RuntimeJSONUtilsTest, SimpleUTF16StringPropertyVal) {
  std::u16string src =
      uR"(
{"prop1": 45, "x": "hi"}
)";
  hermes::UTF16Stream stream{
      llvh::ArrayRef<char16_t>(src.data(), src.length())};
  CallResult<HermesValue> parsedObj =
      runtimeJSONParseRef(runtime, std::move(stream));
  auto jsobj = Handle<JSObject>::vmcast(runtime, *parsedObj);
  ASSERT_EQ(2, jsobj->getClass(runtime)->getNumProperties());
  double prop1Val = jsobj->getNamed_RJS(jsobj, runtime, symbolFor("prop1"))
                        ->getHermesValue()
                        .getDouble();
  ASSERT_EQ(45, prop1Val);
  auto xVal = jsobj->getNamed_RJS(jsobj, runtime, symbolFor("x"))
                  ->getHermesValue()
                  .getString()
                  ->getStringRef<char>();
  std::string prop1ValStr = std::string{xVal.data(), xVal.size()};
  ASSERT_EQ("hi", prop1ValStr);
}

TEST_F(RuntimeJSONUtilsTest, LongUTF16StringVal) {
  std::u16string src = u"\"";
  for (size_t i = 0; i < 4096; i++) {
    src += i % 26 + 'a';
  }
  src += u"\"";
  auto srcRef = llvh::ArrayRef<char16_t>(src.data(), src.size());
  hermes::UTF16Stream stream{srcRef};
  CallResult<HermesValue> parsedStr =
      runtimeJSONParseRef(runtime, std::move(stream));
  auto strPrim = Handle<StringPrimitive>::vmcast(runtime, *parsedStr);
  auto expectedStrRef = srcRef.slice(1, src.size() - 2);
  ASSERT_EQ(expectedStrRef.size(), strPrim->getStringLength());
  for (size_t i = 0; i < expectedStrRef.size(); i++) {
    ASSERT_TRUE(strPrim->at(i) == expectedStrRef[i]);
  }
}

TEST_F(RuntimeJSONUtilsTest, LongUTF8StringVal) {
  std::string src = "\"";
  for (size_t i = 0; i < 4096; i++) {
    src += i % 26 + 'a';
  }
  src += "\"";
  auto srcRef = llvh::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(src.data()), src.size());
  hermes::UTF16Stream stream{srcRef};
  CallResult<HermesValue> parsedStr =
      runtimeJSONParseRef(runtime, std::move(stream));
  auto strPrim = Handle<StringPrimitive>::vmcast(runtime, *parsedStr);
  auto expectedStrRef = srcRef.slice(1, src.size() - 2);
  ASSERT_EQ(expectedStrRef.size(), strPrim->getStringLength());
  for (size_t i = 0; i < expectedStrRef.size(); i++) {
    ASSERT_TRUE(strPrim->at(i) == expectedStrRef[i]);
  }
}

TEST_F(RuntimeJSONUtilsTest, LongUTF8StringPropertyVal) {
  std::string valSrc = "\"";
  for (size_t i = 0; i < 4096; i++) {
    valSrc += i % 26 + 'a';
  }
  valSrc += "\"";
  auto valRef = llvh::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(valSrc.data()), valSrc.size());
  auto expectedStrRef = valRef.slice(1, valSrc.size() - 2);
  std::string src = R"({"prop1": )" + valSrc + "}";
  auto srcRef = llvh::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(src.data()), src.size());
  hermes::UTF16Stream stream{srcRef};
  CallResult<HermesValue> parsedObj =
      runtimeJSONParseRef(runtime, std::move(stream));
  auto obj = Handle<JSObject>::vmcast(runtime, *parsedObj);
  auto prop1Val = obj->getNamed_RJS(obj, runtime, symbolFor("prop1"))
                      ->getHermesValue()
                      .getString();
  ASSERT_EQ(expectedStrRef.size(), prop1Val->getStringLength());
  for (size_t i = 0; i < expectedStrRef.size(); i++) {
    ASSERT_TRUE(prop1Val->at(i) == expectedStrRef[i]);
  }
}

TEST_F(RuntimeJSONUtilsTest, LongUTF8StringValWithEscapes) {
  std::string src = "\"";
  std::string expectedStr = "";
  for (size_t i = 0; i < 4096; i++) {
    src += i % 26 + 'a';
    expectedStr += i % 26 + 'a';
  }
  src += "\\n";
  expectedStr += (char)10;
  for (size_t i = 0; i < 4096; i++) {
    src += i % 26 + 'a';
    expectedStr += i % 26 + 'a';
  }
  src += "\"";
  auto srcRef = llvh::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(src.data()), src.size());
  auto expectedStrRef = llvh::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(expectedStr.data()),
      expectedStr.size());
  hermes::UTF16Stream stream{srcRef};
  CallResult<HermesValue> parsedStr =
      runtimeJSONParseRef(runtime, std::move(stream));
  auto strPrim = Handle<StringPrimitive>::vmcast(runtime, *parsedStr);
  ASSERT_EQ(expectedStrRef.size(), strPrim->getStringLength());
  for (size_t i = 0; i < expectedStrRef.size(); i++) {
    ASSERT_TRUE(strPrim->at(i) == expectedStrRef[i]);
  }
}

TEST_F(RuntimeJSONUtilsTest, LongUTF8StringValWithConversions) {
  std::string src = "\"";
  std::u16string expectedStr = u"";
  for (size_t i = 0; i < 4096; i++) {
    src += i % 26 + 'a';
    expectedStr += i % 26 + 'a';
  }
  // Unicode Character “Ø” (U+00D8)
  src += 0xC3;
  src += 0x98;
  expectedStr += 0x00D8;
  for (size_t i = 0; i < 4096; i++) {
    src += i % 26 + 'a';
    expectedStr += i % 26 + 'a';
  }
  // Unicode Character “𠜎” (U+2070E)
  src += 0xF0;
  src += 0xA0;
  src += 0x9C;
  src += 0x8E;
  expectedStr += 0xD841;
  expectedStr += 0xDF0E;
  src += "\"";
  auto srcRef = llvh::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(src.data()), src.size());
  auto expectedStrRef = llvh::ArrayRef<char16_t>(
      reinterpret_cast<const char16_t *>(expectedStr.data()),
      expectedStr.size());
  hermes::UTF16Stream stream{srcRef};
  CallResult<HermesValue> parsedStr =
      runtimeJSONParseRef(runtime, std::move(stream));
  auto strPrim = Handle<StringPrimitive>::vmcast(runtime, *parsedStr);
  ASSERT_EQ(expectedStrRef.size(), strPrim->getStringLength());
  for (size_t i = 0; i < expectedStrRef.size(); i++) {
    ASSERT_TRUE(strPrim->at(i) == expectedStrRef[i]);
  }
}

TEST_F(RuntimeJSONUtilsTest, UTF8NumberWithEmptyFraction) {
  auto parse = [this](const std::string &src) {
    auto srcRef = llvh::ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t *>(src.data()), src.size());
    return runtimeJSONParseRef(runtime, hermes::UTF16Stream{srcRef});
  };
  for (const char *src : {"1.", "[1.e5]"}) {
    EXPECT_EQ(ExecutionStatus::EXCEPTION, parse(src).getStatus()) << src;
    runtime.clearThrownValue();
  }
  auto res = parse("1.5");
  ASSERT_RETURNED(res.getStatus());
  EXPECT_EQ(1.5, res->getNumber());
}

} // namespace

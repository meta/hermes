/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSONLexer.h"

#include "hermes/Support/FastArraySearch.h"
#include "hermes/VM/StringPrimitive.h"

#include "hermes/Support/BuildTable256.h"
#include "hermes/Support/FastStrToDouble.h"

namespace hermes {
namespace vm {

static constexpr JSONTokenKind tokenTableBuilder(uint8_t ch) {
  switch (ch) {
    case '\t':
    case '\r':
    case '\n':
    case ' ':
      return JSONTokenKind::Whitespace;
    case '"':
      return JSONTokenKind::String;
    case ',':
      return JSONTokenKind::Comma;
    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      return JSONTokenKind::Number;
    case ':':
      return JSONTokenKind::Colon;
    case '[':
      return JSONTokenKind::LSquare;
    case ']':
      return JSONTokenKind::RSquare;
    case '{':
      return JSONTokenKind::LBrace;
    case '}':
      return JSONTokenKind::RBrace;
    case 'f':
      return JSONTokenKind::False;
    case 'n':
      return JSONTokenKind::Null;
    case 't':
      return JSONTokenKind::True;
    default:
      return JSONTokenKind::Error;
  }
}

/// This table maps an ASCII character to a JSONTokenKind.
static const constexpr JSONTokenKind TOKEN_TABLE[256] = {
#define TABLE_ELEMENT(N) tokenTableBuilder(N),
    BUILD_TABLE_256(TABLE_ELEMENT)
#undef TABLE_ELEMENT
};

template <EncodingKind Kind>
ExecutionStatus JSONLexer<Kind>::advance() {
  auto res = advanceHelper<StrAsValue>();
  assert(
      (res == ExecutionStatus::EXCEPTION ||
       (token_.getKind() != JSONTokenKind::Whitespace &&
        token_.getKind() != JSONTokenKind::Error)) &&
      "Error and Whitespace should never be set after a successful advance");
  return res;
}

template <EncodingKind Kind>
ExecutionStatus JSONLexer<Kind>::advanceStrAsSymbol() {
  auto res = advanceHelper<StrAsSymbol>();
  assert(
      (res == ExecutionStatus::EXCEPTION ||
       (token_.getKind() != JSONTokenKind::Whitespace &&
        token_.getKind() != JSONTokenKind::Error)) &&
      "Error and Whitespace should never be set after a successful advance");
  return res;
}

template <EncodingKind Kind>
template <typename ForKey>
ExecutionStatus JSONLexer<Kind>::advanceHelper() {
  // Skip whitespaces.
  JSONTokenKind curKind;
  CharT curVal;
  while (true) {
    if (LLVM_UNLIKELY(!hasChar())) {
      token_.setEof();
      return ExecutionStatus::RETURNED;
    }
    curVal = *iter_.cur;
    if constexpr (sizeof(CharT) > 1) {
      if (LLVM_UNLIKELY(curVal >= 0xFF)) {
        token_.setFirstChar(curVal);
        return errorWithChar(u"Unexpected character: ", curVal);
      }
    }
    curKind = TOKEN_TABLE[(uint8_t)curVal];
    if (curKind == JSONTokenKind::Whitespace) {
      ++iter_.cur;
    } else {
      break;
    }
  }
  token_.setPunctuator(curKind);
  token_.setFirstChar(curVal);

  assert(
      (curKind != JSONTokenKind::Whitespace) &&
      (curKind != JSONTokenKind::Eof) && (curKind != JSONTokenKind::None) &&
      "invalid token");

  using HandlerFn = ExecutionStatus (JSONLexer<Kind>::*)();
  static constexpr HandlerFn dispatchTable[] = {
      // JSONTokenKind::String
      &JSONLexer<Kind>::scanString<ForKey>,
      // JSONTokenKind::Number
      &JSONLexer<Kind>::scanNumber,
      // JSONTokenKind::True
      &JSONLexer<Kind>::scanTrue,
      // JSONTokenKind::False
      &JSONLexer<Kind>::scanFalse,
      // JSONTokenKind::Null
      &JSONLexer<Kind>::scanNull,
      // JSONTokenKind::Comma
      &JSONLexer<Kind>::bumpIterator,
      // JSONTokenKind::Colon
      &JSONLexer<Kind>::bumpIterator,
      // JSONTokenKind::LBrace
      &JSONLexer<Kind>::bumpIterator,
      // JSONTokenKind::RBrace
      &JSONLexer<Kind>::bumpIterator,
      // JSONTokenKind::LSquare
      &JSONLexer<Kind>::bumpIterator,
      // JSONTokenKind::RSquare
      &JSONLexer<Kind>::bumpIterator,
      // JSONTokenKind::Error
      &JSONLexer<Kind>::handleError,
  };
  return (this->*dispatchTable[static_cast<uint8_t>(curKind)])();
}

template <EncodingKind Kind>
CallResult<char16_t> JSONLexer<Kind>::consumeUnicode() {
  uint16_t val = 0;
  for (unsigned i = 0; i < 4; ++i) {
    if (!hasChar()) {
      return error("Unexpected end of input");
    }
    int ch = *iter_.cur | 32;
    if (ch >= '0' && ch <= '9') {
      ch -= '0';
    } else if (ch >= 'a' && ch <= 'f') {
      ch -= 'a' - 10;
    } else {
      return errorWithChar(u"Invalid unicode point character: ", *iter_.cur);
    }
    val = (val << 4) + ch;
    ++iter_.cur;
  }

  return static_cast<char16_t>(val);
}

template <EncodingKind Kind>
ExecutionStatus JSONLexer<Kind>::scanNumber() {
  // Mark the beginning of the number contents.
  const CharT *beginNumContents;
  if constexpr (Traits::UsesRawPtr) {
    beginNumContents = iter_.cur;
  } else {
    iter_.cur.beginCapture();
  }

  // Check for initial negative sign.
  double multiplier = 1;
  if (*iter_.cur == '-') {
    multiplier = -1;
    ++iter_.cur;
    // If there is a leading minus sign, there must be a digit following it.
    if (LLVM_UNLIKELY(!hasChar() || *iter_.cur < '0' || *iter_.cur > '9')) {
      return error("No digits after minus sign in JSON number");
    }
  }

  bool isTrivialInteger = true;
  uint64_t integer = 0;
  constexpr uint64_t kFirstIntThatCanOverflow = (UINT64_MAX - 9) / 10;
  while (hasChar()) {
    auto ch = *iter_.cur;
    if (ch >= '0' && ch <= '9') {
      if (integer > kFirstIntThatCanOverflow) {
        isTrivialInteger = false;
      }
      integer = integer * 10 + (ch - '0');
    } else if (ch == '-' || ch == '+' || ch == '.' || (ch | 32) == 'e') {
      isTrivialInteger = false;
    } else {
      break;
    }
    ++iter_.cur;
  }

  // Obtain a view over the number contents.
  llvh::ArrayRef<CharT> numRef;
  if constexpr (Traits::UsesRawPtr) {
    numRef = llvh::ArrayRef<CharT>{beginNumContents, iter_.cur};
  } else {
    numRef = iter_.cur.endCapture();
  }

  // Check for illegal leading 0 in the integer part, which follows the
  // optional minus sign.
  assert(
      numRef.size() > 0 &&
      "scanNumber must be called on a number-looking char");
  llvh::ArrayRef<CharT> intRef = multiplier < 0 ? numRef.drop_front() : numRef;
  if (LLVM_UNLIKELY(
          intRef.size() > 1 && intRef[0] == '0' && intRef[1] >= '0' &&
          intRef[1] <= '9')) {
    // The integer part cannot start with 0, unless it's 0.
    return errorWithChar(u"Unexpected character in number: ", intRef[1]);
  }

  if (isTrivialInteger) {
    token_.setNumber((double)integer * multiplier);
    return ExecutionStatus::RETURNED;
  }

  StrToDoubleParseResult<CharT> parseRes = fastStrToDouble(numRef);
  if (LLVM_UNLIKELY(parseRes.ptr != numRef.end())) {
    return error("Invalid number input");
  }
  token_.setNumber(parseRes.value);
  return ExecutionStatus::RETURNED;
}

template <EncodingKind Kind>
template <typename ForKey>
ExecutionStatus JSONLexer<Kind>::scanString() {
  assert(*iter_.cur == '"');
  // Advance over opening quote.
  ++iter_.cur;
  const CharT *beginNonEscaped;
  if constexpr (Traits::UsesRawPtr) {
    beginNonEscaped = iter_.cur;
  } else {
    iter_.cur.beginCapture();
  }
  hermes::JenkinsHash hash = hermes::JenkinsHashInit;

  // This loop can only handle simple strings that have no escapes. If an escape
  // is encountered, iteration stops and the string is finished processing
  // below.
  while (true) {
    if (LLVM_UNLIKELY(!hasChar())) {
      return error("Unexpected end of input");
    }

    // SIMD fast scan: skip over runs of normal characters in bulk.
    if constexpr (Traits::UsesRawPtr) {
      const CharT *scanEnd;
      if constexpr (sizeof(CharT) == 1)
        scanEnd = scanJsonEscapeU8(iter_.cur, iter_.end);
      else
        scanEnd = scanJsonEscapeU16(iter_.cur, iter_.end);

      if (scanEnd > iter_.cur) {
        if constexpr (ForKey::value) {
          for (const CharT *p = iter_.cur; p < scanEnd; ++p)
            hash = hermes::updateJenkinsHash(hash, *p);
        }
        iter_.cur = scanEnd;
        if (!hasChar())
          return error("Unexpected end of input");
      }
    }

    CharT curVal = *iter_.cur;
    if (curVal == '"') {
      // Reached the end of string.
      llvh::ArrayRef<CharT> strRef;
      if constexpr (Traits::UsesRawPtr) {
        strRef = llvh::ArrayRef<CharT>{beginNonEscaped, iter_.cur};
      } else {
        strRef = iter_.cur.endCapture();
      }
      ++iter_.cur;
      if constexpr (ForKey::value) {
        auto symRes =
            runtime_.getIdentifierTable().getSymbolID(runtime_, strRef, hash);
        if (symRes == ExecutionStatus::EXCEPTION)
          return ExecutionStatus::EXCEPTION;
        token_.setSymbol(*symRes);
        return ExecutionStatus::RETURNED;
      }
      auto strRes = StringPrimitive::create(runtime_, strRef);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      token_.setString(vmcast<StringPrimitive>(*strRes));
      return ExecutionStatus::RETURNED;
    } else if (LLVM_UNLIKELY(curVal <= '\u001F')) {
      return error(u"U+0000 thru U+001F is not allowed in string");
    }
    if (LLVM_LIKELY(curVal != '\\')) {
      if constexpr (ForKey::value) {
        hash = hermes::updateJenkinsHash(hash, curVal);
      }
      ++iter_.cur;
      continue;
    }
    // escape found, break out.
    break;
  }

  // We reach this point after an escape has occured. This means that the
  // contents of the string we are parsing don't actually exist byte for byte in
  // the input string. Instead, we must build the contents of the string from
  // scratch. The contents of the string are placed into escapedStr.
  llvh::SmallVector<char16_t, 32> escapedStr;
  // Append all the 'simple' string contents that were observed up to this
  // point.
  if constexpr (Traits::UsesRawPtr) {
    escapedStr.append(beginNonEscaped, iter_.cur);
  } else {
    llvh::ArrayRef<char16_t> nonEscapedRef = iter_.cur.endCapture();
    escapedStr.append(nonEscapedRef.begin(), nonEscapedRef.end());
  }
  while (true) {
    if (LLVM_UNLIKELY(!hasChar())) {
      return error("Unexpected end of input");
    }
    CharT curVal = *iter_.cur;
    if (curVal == '"') {
      // Reached the end of string.
      ++iter_.cur;
      if constexpr (ForKey::value) {
        auto symRes = runtime_.getIdentifierTable().getSymbolID(
            runtime_, llvh::ArrayRef<char16_t>{escapedStr}, hash);
        if (symRes == ExecutionStatus::EXCEPTION)
          return ExecutionStatus::EXCEPTION;
        token_.setSymbol(*symRes);
        return ExecutionStatus::RETURNED;
      }
      auto strRes = StringPrimitive::create(
          runtime_, llvh::ArrayRef<char16_t>{escapedStr});
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      token_.setString(vmcast<StringPrimitive>(*strRes));
      return ExecutionStatus::RETURNED;
    } else if (LLVM_UNLIKELY(curVal <= '\u001F')) {
      return error(u"U+0000 thru U+001F is not allowed in string");
    }
    if (LLVM_LIKELY(curVal != '\\')) {
      escapedStr.push_back(curVal);
      if constexpr (ForKey::value) {
        hash = hermes::updateJenkinsHash(hash, curVal);
      }
      ++iter_.cur;
      continue;
    }
    // Advance over starting backslash.
    ++iter_.cur;
    if (!hasChar()) {
      return error("Unexpected end of input");
    }
    switch (*iter_.cur) {
      case '"':
        escapedStr.push_back('"');
        ++iter_.cur;
        break;
      case '/':
        escapedStr.push_back('/');
        ++iter_.cur;
        break;
      case '\\':
        escapedStr.push_back('\\');
        ++iter_.cur;
        break;
      case 'b':
        // Backspace (Unicode U+0008)
        escapedStr.push_back(0x08);
        ++iter_.cur;
        break;
      case 'f':
        // Form feed (Unicode U+000C)
        escapedStr.push_back(0x0C);
        ++iter_.cur;
        break;
      case 'n':
        // Line feed (Unicode U+000A)
        escapedStr.push_back(0x0A);
        ++iter_.cur;
        break;
      case 'r':
        // Carriage return (Unicode U+000D)
        escapedStr.push_back(0x0D);
        ++iter_.cur;
        break;
      case 't':
        // Horiztonal tab (Unicode U+0009)
        escapedStr.push_back(0x09);
        ++iter_.cur;
        break;
      case 'u': {
        ++iter_.cur;
        CallResult<char16_t> cr = consumeUnicode();
        if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
          return ExecutionStatus::EXCEPTION;
        }
        escapedStr.push_back(*cr);
        break;
      }

      default:
        return errorWithChar(u"Invalid escape sequence: ", *iter_.cur);
    }
    char16_t escapedCharVal = escapedStr.back();
    if constexpr (ForKey::value) {
      hash = hermes::updateJenkinsHash(hash, escapedCharVal);
    }
  }
  return error("Unexpected end of input");
}

template <EncodingKind Kind>
ExecutionStatus JSONLexer<Kind>::scanWord(const char *word) {
  while (*word && hasChar()) {
    if (*iter_.cur != *word) {
      return errorWithChar(u"Unexpected character: ", *iter_.cur);
    }
    ++iter_.cur;
    ++word;
  }
  if (*word) {
    return error(u"Unexpected end of input");
  }
  return ExecutionStatus::RETURNED;
}

template class JSONLexer<EncodingKind::ASCII>;
template class JSONLexer<EncodingKind::UTF8>;
template class JSONLexer<EncodingKind::UTF16>;

} // namespace vm
} // namespace hermes

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TextDecoder.h"

#include "../JSIUtils.h"
#include "TextDecoderUtils.h"

#include "hermes/Platform/Unicode/CharacterProperties.h"
#include "hermes/Support/UTF8.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace facebook {
namespace hermes {
namespace {

using ::hermes::UNICODE_REPLACEMENT_CHARACTER;

/// NativeState subclass for TextDecoder instances.
/// Stores encoding configuration and streaming state.
class TextDecoderNativeState : public jsi::NativeState {
 public:
  TextDecoderEncoding encoding;
  bool fatal;
  bool ignoreBOM;
  // Streaming state
  uint8_t pendingBytes[4];
  size_t pendingCount;
  bool bomSeen;

  TextDecoderNativeState(
      TextDecoderEncoding enc,
      bool fatalFlag,
      bool ignoreBOMFlag)
      : encoding(enc),
        fatal(fatalFlag),
        ignoreBOM(ignoreBOMFlag),
        pendingCount(0),
        bomSeen(false) {}
};

/// Validate that 'thisVal' is a TextDecoder instance (has our NativeState).
/// Returns the NativeState pointer, or throws a TypeError if invalid.
inline TextDecoderNativeState *getTextDecoderState(
    jsi::Runtime &rt,
    const jsi::Value &thisVal,
    const char *methodName) {
  if (!thisVal.isObject()) {
    throwTypeError(
        rt,
        (std::string(methodName) + " called on non-TextDecoder object")
            .c_str());
  }
  auto obj = thisVal.asObject(rt);
  if (!obj.hasNativeState<TextDecoderNativeState>(rt)) {
    throwTypeError(
        rt,
        (std::string(methodName) + " called on non-TextDecoder object")
            .c_str());
  }
  return obj.getNativeState<TextDecoderNativeState>(rt).get();
}

/// Get input bytes from a value that can be ArrayBuffer, TypedArray, or
/// DataView. Returns an empty view for undefined/null. The result roots the
/// backing ArrayBuffer, so it must outlive the use of its bytes.
/// Throws a TypeError if the value is not a valid buffer type.
/// Throws JSINativeException (to be caught by caller) if buffer is detached.
TypedArrayBufferInfo getInputBytes(jsi::Runtime &rt, const jsi::Value &val) {
  if (val.isUndefined() || val.isNull()) {
    return TypedArrayBufferInfo();
  }

  if (!val.isObject()) {
    throwTypeError(
        rt,
        "TextDecoder.prototype.decode() requires an ArrayBuffer or ArrayBufferView");
  }

  jsi::Object obj = val.asObject(rt);

  // Check if it's an ArrayBuffer directly
  if (obj.isArrayBuffer(rt)) {
    jsi::ArrayBuffer ab = obj.getArrayBuffer(rt);
    // data(rt) throws JSINativeException if detached
    uint8_t *data = ab.data(rt);
    size_t size = ab.size(rt);
    return TypedArrayBufferInfo(jsi::Value(std::move(ab)), data, size);
  }

  // Check for TypedArray or DataView using shared utility
  return getTypedArrayBuffer(
      rt,
      val,
      "TextDecoder.prototype.decode() requires an ArrayBuffer or ArrayBufferView",
      "TextDecoder.prototype.decode called on a detached ArrayBuffer");
}

/// Native helper called by the JS constructor to install NativeState.
/// This marks the object as a valid TextDecoder instance.
jsi::Value textDecoderInit(
    jsi::Runtime &rt,
    const jsi::Value &,
    const jsi::Value *args,
    size_t count) {
  jsi::Object self = args[0].asObject(rt);

  // Parse encoding label (default is "utf-8")
  TextDecoderEncoding encoding = TextDecoderEncoding::UTF8;
  if (count > 1 && !args[1].isUndefined()) {
    std::string labelStr = args[1].toString(rt).utf8(rt);
    auto parsedEncoding = parseEncodingLabel(labelStr);
    if (!parsedEncoding) {
      throwRangeError(rt, ("Unknown encoding: " + labelStr).c_str());
    }
    encoding = *parsedEncoding;
  }

  // Parse options (third argument)
  bool fatal = false;
  bool ignoreBOM = false;
  if (count > 2 && !args[2].isUndefined() && args[2].isObject()) {
    jsi::Object options = args[2].asObject(rt);

    jsi::Value fatalVal = options.getProperty(rt, "fatal");
    if (!fatalVal.isUndefined()) {
      fatal = fatalVal.asBool();
    }

    jsi::Value ignoreBOMVal = options.getProperty(rt, "ignoreBOM");
    if (!ignoreBOMVal.isUndefined()) {
      ignoreBOM = ignoreBOMVal.asBool();
    }
  }

  // Create and attach NativeState
  auto state =
      std::make_shared<TextDecoderNativeState>(encoding, fatal, ignoreBOM);
  self.setNativeState(rt, state);

  return jsi::Value::undefined();
}

/// Implementation of TextDecoder.prototype.decode()
jsi::Value textDecoderDecode(
    jsi::Runtime &rt,
    const jsi::Value &thisVal,
    const jsi::Value *args,
    size_t count) {
  TextDecoderNativeState *state =
      getTextDecoderState(rt, thisVal, "TextDecoder.prototype.decode()");

  // Parse stream option
  bool stream = false;
  if (count > 1 && !args[1].isUndefined() && args[1].isObject()) {
    jsi::Object options = args[1].asObject(rt);
    jsi::Value streamVal = options.getProperty(rt, "stream");
    if (!streamVal.isUndefined()) {
      stream = streamVal.asBool();
    }
  }

  // Get input bytes. inputInfo roots the backing ArrayBuffer, so it has to
  // outlive every use of 'bytes': building the result string allocates, and a
  // GC in that window would otherwise run the buffer's finalizer and free the
  // storage 'bytes' points into.
  TypedArrayBufferInfo inputInfo;
  const uint8_t *bytes = nullptr;
  size_t length = 0;
  if (count > 0 && !args[0].isUndefined()) {
    try {
      inputInfo = getInputBytes(rt, args[0]);
    } catch (const jsi::JSINativeException &) {
      throwTypeError(
          rt, "TextDecoder.prototype.decode called on a detached ArrayBuffer");
    }
    bytes = inputInfo.data();
    length = inputInfo.byteLength();
  }

  // Fast path for single-byte encodings: single-pass decode with ASCII
  // optimization. No pending bytes possible for single-byte encodings.
  if (isSingleByteEncoding(state->encoding)) {
    if (bytes == nullptr || length == 0) {
      return jsi::String::createFromAscii(rt, "", 0);
    }
    size_t tableIndex =
        static_cast<uint8_t>(state->encoding) - kFirstSingleByteEncoding;
    const char16_t *table = kSingleByteEncodings.at(tableIndex);

    // Single pass: decode and check if all ASCII simultaneously
    std::u16string decoded;
    decoded.reserve(length);
    bool allAscii = true;

    for (size_t i = 0; i < length; ++i) {
      uint8_t b = bytes[i];
      if (b < 0x80) {
        decoded.push_back(static_cast<char16_t>(b));
      } else {
        allAscii = false;
        char16_t cp = table[b - 0x80];
        if (state->fatal && cp == UNICODE_REPLACEMENT_CHARACTER) {
          throwTypeError(rt, "Invalid byte sequence");
        }
        decoded.push_back(cp);
      }
    }

    if (allAscii) {
      return jsi::String::createFromAscii(
          rt, reinterpret_cast<const char *>(bytes), length);
    }
    return jsi::String::createFromUtf16(rt, decoded.data(), decoded.size());
  }

  // Fast path for UTF-8 ASCII-only input with no pending bytes
  if (state->encoding == TextDecoderEncoding::UTF8 &&
      state->pendingCount == 0 && bytes != nullptr) {
    const uint8_t *asciiBytes = bytes;
    size_t asciiLength = length;
    // Skip UTF-8 BOM if present, not ignored, and not already seen.
    if (!state->ignoreBOM && !state->bomSeen && asciiLength >= 3 &&
        asciiBytes[0] == 0xEF && asciiBytes[1] == 0xBB &&
        asciiBytes[2] == 0xBF) {
      asciiBytes += 3;
      asciiLength -= 3;
    }
    // Check if all bytes are ASCII
    bool allASCII = true;
    for (size_t i = 0; i < asciiLength; ++i) {
      if (asciiBytes[i] >= 0x80) {
        allASCII = false;
        break;
      }
    }
    if (allASCII) {
      // Update or reset streaming state
      if (stream) {
        if (asciiLength > 0 && !state->bomSeen) {
          state->bomSeen = true;
        }
      } else {
        // Reset state for next decode call
        state->bomSeen = false;
      }
      return jsi::String::createFromAscii(
          rt, reinterpret_cast<const char *>(asciiBytes), asciiLength);
    }
  }

  // Combine pending bytes with new input
  const uint8_t *inputBytes;
  size_t inputLength;
  std::vector<uint8_t> combined;

  if (state->pendingCount > 0) {
    combined.reserve(state->pendingCount + length);
    combined.insert(
        combined.end(),
        state->pendingBytes,
        state->pendingBytes + state->pendingCount);
    if (bytes && length > 0) {
      combined.insert(combined.end(), bytes, bytes + length);
    }
    inputBytes = combined.data();
    inputLength = combined.size();
  } else {
    inputBytes = bytes;
    inputLength = length;
  }

  uint8_t newPendingBytes[4];
  size_t newPendingCount = 0;
  bool newBOMSeen = state->bomSeen;
  std::u16string decoded;
  DecodeError err = DecodeError::None;

  if (state->encoding == TextDecoderEncoding::UTF8) {
    err = decodeUTF8(
        inputBytes,
        inputLength,
        state->fatal,
        state->ignoreBOM,
        stream,
        state->bomSeen,
        &decoded,
        newPendingBytes,
        &newPendingCount,
        &newBOMSeen);
  } else if (state->encoding == TextDecoderEncoding::UTF16LE) {
    err = decodeUTF16(
        inputBytes,
        inputLength,
        state->fatal,
        state->ignoreBOM,
        false,
        stream,
        state->bomSeen,
        &decoded,
        newPendingBytes,
        &newPendingCount,
        &newBOMSeen);
  } else if (state->encoding == TextDecoderEncoding::UTF16BE) {
    err = decodeUTF16(
        inputBytes,
        inputLength,
        state->fatal,
        state->ignoreBOM,
        true,
        stream,
        state->bomSeen,
        &decoded,
        newPendingBytes,
        &newPendingCount,
        &newBOMSeen);
  }

  // Update or clear streaming state
  if (stream) {
    state->pendingCount = newPendingCount;
    for (size_t i = 0; i < newPendingCount; ++i) {
      state->pendingBytes[i] = newPendingBytes[i];
    }
    state->bomSeen = newBOMSeen;
  } else {
    state->pendingCount = 0;
    state->bomSeen = false;
  }

  if (err != DecodeError::None) {
    switch (err) {
      case DecodeError::InvalidSequence:
        throwTypeError(rt, "Invalid byte sequence");
      case DecodeError::InvalidSurrogate:
        throwTypeError(rt, "Invalid UTF-16: lone surrogate");
      case DecodeError::OddByteCount:
        throwTypeError(rt, "Invalid UTF-16 data (odd byte count)");
      default:
        throwTypeError(rt, "Decoding error");
    }
  }

  return jsi::String::createFromUtf16(rt, decoded.data(), decoded.size());
}

/// Implementation of TextDecoder.prototype.encoding getter
/// Note: JS calls this as nativeGetEncoding(this), so the TextDecoder
/// instance is passed as args[0], not as thisVal.
jsi::Value textDecoderGetEncoding(
    jsi::Runtime &rt,
    const jsi::Value &,
    const jsi::Value *args,
    size_t count) {
  if (count < 1) {
    throw jsi::JSError(
        rt, "TextDecoder.prototype.encoding called with no arguments");
  }
  TextDecoderNativeState *state =
      getTextDecoderState(rt, args[0], "TextDecoder.prototype.encoding");
  return jsi::String::createFromAscii(rt, getEncodingName(state->encoding));
}

/// Implementation of TextDecoder.prototype.fatal getter
/// Note: JS calls this as nativeGetFatal(this), so the TextDecoder
/// instance is passed as args[0], not as thisVal.
jsi::Value textDecoderGetFatal(
    jsi::Runtime &rt,
    const jsi::Value &,
    const jsi::Value *args,
    size_t count) {
  if (count < 1) {
    throw jsi::JSError(
        rt, "TextDecoder.prototype.fatal called with no arguments");
  }
  TextDecoderNativeState *state =
      getTextDecoderState(rt, args[0], "TextDecoder.prototype.fatal");
  return jsi::Value(state->fatal);
}

/// Implementation of TextDecoder.prototype.ignoreBOM getter
/// Note: JS calls this as nativeGetIgnoreBOM(this), so the TextDecoder
/// instance is passed as args[0], not as thisVal.
jsi::Value textDecoderGetIgnoreBOM(
    jsi::Runtime &rt,
    const jsi::Value &,
    const jsi::Value *args,
    size_t count) {
  if (count < 1) {
    throw jsi::JSError(
        rt, "TextDecoder.prototype.ignoreBOM called with no arguments");
  }
  TextDecoderNativeState *state =
      getTextDecoderState(rt, args[0], "TextDecoder.prototype.ignoreBOM");
  return jsi::Value(state->ignoreBOM);
}

} // namespace

void installTextDecoder(jsi::Runtime &rt, jsi::Object &extensions) {
  // Get the setup function from the precompiled extensions object
  jsi::Function setup = extensions.getPropertyAsFunction(rt, "TextDecoder");

  // Create native helper functions
  jsi::Function nativeInit = jsi::Function::createFromHostFunction(
      rt, jsi::PropNameID::forAscii(rt, "textDecoderInit"), 3, textDecoderInit);

  jsi::Function nativeDecode = jsi::Function::createFromHostFunction(
      rt,
      jsi::PropNameID::forAscii(rt, "textDecoderDecode"),
      2,
      textDecoderDecode);

  jsi::Function nativeGetEncoding = jsi::Function::createFromHostFunction(
      rt,
      jsi::PropNameID::forAscii(rt, "textDecoderGetEncoding"),
      1,
      textDecoderGetEncoding);

  jsi::Function nativeGetFatal = jsi::Function::createFromHostFunction(
      rt,
      jsi::PropNameID::forAscii(rt, "textDecoderGetFatal"),
      1,
      textDecoderGetFatal);

  jsi::Function nativeGetIgnoreBOM = jsi::Function::createFromHostFunction(
      rt,
      jsi::PropNameID::forAscii(rt, "textDecoderGetIgnoreBOM"),
      1,
      textDecoderGetIgnoreBOM);

  // Call the setup function with our native helpers
  setup.call(
      rt,
      nativeInit,
      nativeDecode,
      nativeGetEncoding,
      nativeGetFatal,
      nativeGetIgnoreBOM);
}

} // namespace hermes
} // namespace facebook

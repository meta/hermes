/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TextEncoder.h"

#include "JSIUtils.h"

#include "hermes/Support/UTF8.h"
#include "llvh/ADT/ArrayRef.h"

#include <cstring>

namespace facebook {
namespace hermes {
namespace {

/// NativeState subclass used to identify TextEncoder instances.
class TextEncoderNativeState : public jsi::NativeState {
 public:
  TextEncoderNativeState() = default;
};

/// Validate that 'thisVal' is a TextEncoder instance (has our NativeState).
/// Throws a TypeError if not.
inline void validateTextEncoder(
    jsi::Runtime &rt,
    const jsi::Value &thisVal,
    const char *methodName) {
  if (!thisVal.isObject() ||
      !thisVal.asObject(rt).hasNativeState<TextEncoderNativeState>(rt)) {
    throwTypeError(
        rt,
        (std::string(methodName) + " called on non-TextEncoder object")
            .c_str());
  }
}

/// Native helper called by the JS constructor to install NativeState.
/// This marks the object as a valid TextEncoder instance.
jsi::Value textEncoderInit(
    jsi::Runtime &rt,
    const jsi::Value &,
    const jsi::Value *args,
    size_t count) {
  // Defensive check: we expect exactly 1 argument from our JS constructor.
  if (count != 1) {
    throw jsi::JSError(rt, "textEncoderInit requires exactly 1 argument");
  }
  jsi::Object self = args[0].asObject(rt);
  self.setNativeState(rt, std::make_shared<TextEncoderNativeState>());
  return jsi::Value::undefined();
}

/// Implementation of TextEncoder.prototype.encode()
/// Encodes a string to UTF-8 and returns a Uint8Array.
jsi::Value textEncoderEncode(
    jsi::Runtime &rt,
    const jsi::Value &thisVal,
    const jsi::Value *args,
    size_t count) {
  validateTextEncoder(rt, thisVal, "TextEncoder.prototype.encode()");

  // Get Uint8Array constructor from intrinsics
  const jsi::Function &uint8ArrayCtor = getIntrinsics(rt).uint8Array;

  // Get input string (default to empty string if no argument)
  if (count == 0 || args[0].isUndefined()) {
    return uint8ArrayCtor.callAsConstructor(rt, 0);
  }

  // Convert to UTF-8. jsi::String::utf8() already handles invalid surrogates
  // with U+FFFD replacement, which is correct per the WHATWG Encoding Standard.
  std::string utf8 = args[0].toString(rt).utf8(rt);

  if (utf8.empty()) {
    return uint8ArrayCtor.callAsConstructor(rt, 0);
  }

  // Create a buffer holding the UTF-8 data
  auto buffer = std::make_shared<UTF8Buffer>(std::move(utf8));

  // Create ArrayBuffer from the buffer
  jsi::ArrayBuffer arrayBuffer(rt, buffer);

  // Call new Uint8Array(arrayBuffer)
  return uint8ArrayCtor.callAsConstructor(rt, std::move(arrayBuffer));
}

/// Implementation of TextEncoder.prototype.encodeInto()
/// Encodes a string into an existing Uint8Array and returns {read, written}.
jsi::Value textEncoderEncodeInto(
    jsi::Runtime &rt,
    const jsi::Value &thisVal,
    const jsi::Value *args,
    size_t count) {
  validateTextEncoder(rt, thisVal, "TextEncoder.prototype.encodeInto()");

  if (count < 2) {
    throwTypeError(rt, "TextEncoder.encodeInto requires 2 arguments");
  }

  jsi::String inputStr = args[0].toString(rt);

  // Get the destination buffer info (validates and extracts
  // buffer/offset/length)
  TypedArrayBufferInfo destInfo = getTypedArrayBuffer(
      rt,
      args[1],
      "The second argument should be a Uint8Array",
      "TextEncoder.prototype.encodeInto called on a detached ArrayBuffer");

  uint8_t *destStart = destInfo.data();
  uint32_t numRead = 0;
  uint32_t numWritten = 0;
  size_t remainingBytes = destInfo.byteLength();

  // Use getStringData to access internal representation efficiently.
  // ASCII strings can be copied directly (ASCII is a subset of UTF-8).
  // UTF-16 strings need conversion.
  auto cb = [&](bool ascii, const void *data, size_t num) {
    if (remainingBytes == 0 || num == 0) {
      return;
    }

    if (ascii) {
      // ASCII is a strict subset of UTF-8, copy directly
      size_t toCopy = std::min(num, remainingBytes);
      std::memcpy(destStart + numWritten, data, toCopy);
      numRead += static_cast<uint32_t>(toCopy);
      numWritten += static_cast<uint32_t>(toCopy);
      remainingBytes -= toCopy;
    } else {
      // UTF-16 data, convert to UTF-8 directly into the buffer
      llvh::ArrayRef<char16_t> utf16Data(
          static_cast<const char16_t *>(data), num);
      std::pair<uint32_t, uint32_t> result =
          ::hermes::convertUTF16ToUTF8BufferWithReplacements(
              llvh::MutableArrayRef<uint8_t>(
                  destStart + numWritten, remainingBytes),
              utf16Data);
      numRead += result.first;
      numWritten += result.second;
      remainingBytes -= result.second;
    }
  };
  inputStr.getStringData(rt, cb);

  // Return {read, written} object
  jsi::Object resultObj(rt);
  resultObj.setProperty(rt, "read", static_cast<double>(numRead));
  resultObj.setProperty(rt, "written", static_cast<double>(numWritten));
  return resultObj;
}

} // namespace

void installTextEncoder(jsi::Runtime &rt, jsi::Object &extensions) {
  // Get the setup function from the precompiled extensions object
  jsi::Function setup = extensions.getPropertyAsFunction(rt, "TextEncoder");

  // Create native helper functions
  jsi::Function nativeInit = jsi::Function::createFromHostFunction(
      rt, jsi::PropNameID::forAscii(rt, "textEncoderInit"), 1, textEncoderInit);

  jsi::Function nativeEncode = jsi::Function::createFromHostFunction(
      rt,
      jsi::PropNameID::forAscii(rt, "textEncoderEncode"),
      1,
      textEncoderEncode);

  jsi::Function nativeEncodeInto = jsi::Function::createFromHostFunction(
      rt,
      jsi::PropNameID::forAscii(rt, "textEncoderEncodeInto"),
      2,
      textEncoderEncodeInto);

  // Call the setup function with our native helpers
  setup.call(rt, nativeInit, nativeEncode, nativeEncodeInto);
}

} // namespace hermes
} // namespace facebook

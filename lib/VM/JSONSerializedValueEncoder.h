/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "hermes/VM/SerializedValue.h"
#include "jsi/jsi.h"

#include "llvh/Support/ConvertUTF.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hermes::vm {

using JSONValue = ::facebook::jsi::JSONValue;

/// Encodes the JSON subset directly into Hermes' internal structured-clone
/// SerializedValue representation, without constructing a Runtime.
class JSONSerializedValueEncoder {
 public:
  SerializedValue encode(const JSONValue &value) {
    SerializedValue result;
    out_ = &result;
    struct ResetOutput {
      explicit ResetOutput(JSONSerializedValueEncoder &encoder)
          : encoder_(encoder) {}
      ~ResetOutput() {
        encoder_.out_ = nullptr;
      }

      JSONSerializedValueEncoder &encoder_;
    } reset{*this};

    stringIds_.clear();
    writeValue(value);
    return result;
  }

 private:
  SerializedValue *out_{nullptr};
  std::unordered_map<std::string, uint32_t> stringIds_;

  static uint8_t typeByte(SerializedValue::Type type) {
    return static_cast<uint8_t>(type);
  }

  template <typename T>
  static void appendPod(std::vector<uint8_t> &buffer, T value) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
    buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
  }

  static void appendDouble(std::vector<uint8_t> &buffer, double value) {
    uint64_t bits;
    static_assert(sizeof(bits) == sizeof(value), "Unexpected double size");
    std::memcpy(&bits, &value, sizeof(bits));
    appendPod<uint64_t>(buffer, bits);
  }

  static uint32_t checkedUint32(size_t value, const char *message) {
    if (value > std::numeric_limits<uint32_t>::max()) {
      throw std::overflow_error(message);
    }
    return static_cast<uint32_t>(value);
  }

  uint32_t nextRecordID() const {
    return checkedUint32(
        out_->offsets.size(), "Serialized value has too many records");
  }

  void pushOffset(size_t offset) {
    out_->offsets.push_back(
        checkedUint32(offset, "Serialized value buffer is too large"));
  }

  static void appendUTF16(
      std::vector<uint8_t> &buffer,
      std::u16string_view value) {
    buffer.insert(
        buffer.end(),
        reinterpret_cast<const uint8_t *>(value.data()),
        reinterpret_cast<const uint8_t *>(value.data() + value.size()));
  }

  static std::u16string convertUTF8ToUTF16(std::string_view value) {
    if (value.empty()) {
      return {};
    }

    std::u16string out;
    out.resize(value.size());

    const auto *sourceStart =
        reinterpret_cast<const llvh::UTF8 *>(value.data());
    const auto *sourceEnd = sourceStart + value.size();
    auto *targetStart = reinterpret_cast<llvh::UTF16 *>(out.data());
    auto *targetEnd = targetStart + out.size();
    llvh::ConversionResult result = llvh::ConvertUTF8toUTF16(
        &sourceStart,
        sourceEnd,
        &targetStart,
        targetEnd,
        llvh::strictConversion);
    if (result != llvh::ConversionResult::conversionOK) {
      throw std::invalid_argument("Invalid UTF-8 JSON string");
    }

    out.resize(reinterpret_cast<char16_t *>(targetStart) - out.data());
    return out;
  }

  static std::string makeStringKey(const JSONValue &value) {
    std::string key;
    key.push_back(static_cast<char>(value.stringEncoding));
    if (value.stringEncoding == JSONValue::StringEncoding::UTF16) {
      const auto *bytes =
          reinterpret_cast<const char *>(value.utf16StringValue.data());
      key.append(bytes, value.utf16StringValue.size() * sizeof(char16_t));
    } else {
      key.append(value.stringValue);
    }
    return key;
  }

  uint32_t internString(const JSONValue &value) {
    std::string key = makeStringKey(value);
    auto it = stringIds_.find(key);
    if (it != stringIds_.end()) {
      return it->second;
    }

    const uint32_t id = nextRecordID();
    stringIds_.emplace(std::move(key), id);
    pushOffset(out_->strings.size());

    std::u16string utf16Storage;
    const bool ascii = value.stringEncoding == JSONValue::StringEncoding::ASCII;
    if (value.stringEncoding == JSONValue::StringEncoding::UTF8) {
      utf16Storage = convertUTF8ToUTF16(value.stringValue);
    }

    out_->strings.push_back(ascii ? 1 : 0);
    const uint32_t length = ascii
        ? checkedUint32(value.stringValue.size(), "JSON string is too large")
        : value.stringEncoding == JSONValue::StringEncoding::UTF16
        ? checkedUint32(
              value.utf16StringValue.size(), "JSON string is too large")
        : checkedUint32(utf16Storage.size(), "JSON string is too large");
    appendPod<uint32_t>(out_->strings, length);

    if (ascii) {
      out_->strings.insert(
          out_->strings.end(),
          value.stringValue.begin(),
          value.stringValue.end());
    } else {
      while (out_->strings.size() % alignof(char16_t) != 0) {
        out_->strings.push_back(0);
      }
      if (value.stringEncoding == JSONValue::StringEncoding::UTF16) {
        appendUTF16(out_->strings, value.utf16StringValue);
      } else {
        appendUTF16(out_->strings, utf16Storage);
      }
    }

    return id;
  }

  uint32_t internUTF8String(std::string_view value) {
    return internString(JSONValue::utf8String(std::string{value}));
  }

  void writeValue(const JSONValue &value) {
    switch (value.kind) {
      case JSONValue::Kind::Null:
        out_->content.push_back(typeByte(SerializedValue::Type::Null));
        return;
      case JSONValue::Kind::Bool:
        out_->content.push_back(
            typeByte(SerializedValue::Type::PrimitiveBoolean));
        out_->content.push_back(value.boolValue ? 1 : 0);
        return;
      case JSONValue::Kind::Number:
        if (!std::isfinite(value.numberValue)) {
          throw std::invalid_argument("JSON numbers must be finite");
        }
        out_->content.push_back(
            typeByte(SerializedValue::Type::PrimitiveNumber));
        appendDouble(out_->content, value.numberValue);
        return;
      case JSONValue::Kind::String:
        out_->content.push_back(
            typeByte(SerializedValue::Type::PrimitiveString));
        appendPod<uint32_t>(out_->content, internString(value));
        return;
      case JSONValue::Kind::Array:
        writeArray(value.arrayValue);
        return;
      case JSONValue::Kind::Object:
        writeObject(value.objectValue);
        return;
    }
    assert(false && "Unhandled JSON kind");
  }

  uint32_t beginObjectRecord(SerializedValue::Type type) {
    const uint32_t id = nextRecordID();
    pushOffset(out_->content.size());
    out_->content.push_back(typeByte(type));
    appendPod<uint32_t>(out_->content, id);
    return id;
  }

  void writeArray(const std::vector<JSONValue> &array) {
    if (array.size() > std::numeric_limits<uint32_t>::max()) {
      throw std::overflow_error("JSON array is too large");
    }
    beginObjectRecord(SerializedValue::Type::Array);
    const auto len = static_cast<uint32_t>(array.size());
    appendPod<uint32_t>(out_->content, len);
    appendPod<uint32_t>(out_->content, len);
    for (uint32_t i = 0; i < len; ++i) {
      appendPod<uint32_t>(out_->content, i);
      writeValue(array[i]);
    }
    appendPod<uint32_t>(out_->content, 0);
  }

  void writeObject(const JSONValue::Object &object) {
    if (object.size() > std::numeric_limits<uint32_t>::max()) {
      throw std::overflow_error("JSON object has too many properties");
    }
    beginObjectRecord(SerializedValue::Type::Object);
    appendPod<uint32_t>(out_->content, static_cast<uint32_t>(object.size()));
    for (const auto &[key, value] : object) {
      appendPod<uint32_t>(out_->content, internUTF8String(key));
      writeValue(value);
    }
  }
};

} // namespace hermes::vm

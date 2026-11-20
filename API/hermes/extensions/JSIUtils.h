/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Intrinsics.h"

#include "jsi/jsi.h"
#include "llvh/ADT/Optional.h"

#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace facebook {
namespace hermes {

/// A view into an ArrayBuffer's storage, together with the ArrayBuffer itself.
/// Holding the ArrayBuffer is what keeps the storage alive: nothing else may
/// reference the buffer (it can come from a 'buffer' getter), and collecting
/// it runs the finalizer that frees the storage. Callers must therefore keep
/// this object alive for as long as they use data(), in particular across any
/// operation that can allocate and trigger a GC.
class TypedArrayBufferInfo {
 public:
  /// Construct an empty view: no bytes and no buffer.
  TypedArrayBufferInfo() = default;

  /// \param buffer the ArrayBuffer owning the storage, kept alive by this
  ///   object.
  /// \param data the first byte of the view into \p buffer.
  /// \param byteLength the number of bytes in the view.
  TypedArrayBufferInfo(jsi::Value &&buffer, uint8_t *data, size_t byteLength)
      : buffer_(std::move(buffer)), data_(data), byteLength_(byteLength) {}

  TypedArrayBufferInfo(TypedArrayBufferInfo &&) = default;
  TypedArrayBufferInfo &operator=(TypedArrayBufferInfo &&) = default;

  /// \return the first byte of the view, or nullptr if it is empty.
  uint8_t *data() const {
    return data_;
  }

  /// \return the number of bytes in the view.
  size_t byteLength() const {
    return byteLength_;
  }

 private:
  jsi::Value buffer_;
  uint8_t *data_{nullptr};
  size_t byteLength_{0};
};

/// MutableBuffer implementation that owns a std::string of UTF-8 data.
class UTF8Buffer : public jsi::MutableBuffer {
 public:
  explicit UTF8Buffer(std::string &&data) : data_(std::move(data)) {}

  size_t size() const override {
    return data_.size();
  }

  uint8_t *data() override {
    return reinterpret_cast<uint8_t *>(data_.data());
  }

 private:
  std::string data_;
};

/// Try to convert a jsi::Value to a valid unsigned integer of type T.
/// Returns llvh::None if the value is not a number, is NaN, negative,
/// not an integer, exceeds MAX_SAFE_INTEGER, or exceeds the range of T.
template <typename T>
inline llvh::Optional<T> valueToUnsigned(const jsi::Value &val) {
  static_assert(std::is_unsigned<T>::value, "T must be an unsigned type");
  if (!val.isNumber()) {
    return llvh::None;
  }
  double d = val.asNumber();
  // Check non-negative and within MAX_SAFE_INTEGER (2^53 - 1), which is exactly
  // representable as a double. This ensures d could be an exact integer.
  constexpr double maxSafeInteger = 9007199254740991.0; // 2^53 - 1
  if (!(d >= 0 && d <= maxSafeInteger)) {
    return llvh::None;
  }
  // Cast to uint64_t and verify no fractional part was lost.
  auto u = static_cast<uint64_t>(d);
  if (static_cast<double>(u) != d) {
    return llvh::None;
  }
  if (u > std::numeric_limits<T>::max()) {
    return llvh::None;
  }
  return static_cast<T>(u);
}

/// Extract buffer info from a TypedArray-like object.
/// We use duck-typing (checking for buffer/byteOffset/byteLength) rather than
/// instanceof Uint8Array. This is more permissive than the spec requires, but
/// handles cross-realm Uint8Arrays and is consistent with JSI's portable
/// design. The original VM implementation used internal type checks.
/// Throws a TypeError if the object is not a valid TypedArray or is detached.
/// The returned object keeps the backing ArrayBuffer alive; it must outlive
/// every use of its data().
/// \param errorMessage The error message to use if the object is invalid.
/// \param detachedErrorMessage The error message to use if the buffer is
///   detached.
TypedArrayBufferInfo getTypedArrayBuffer(
    jsi::Runtime &rt,
    const jsi::Value &val,
    const char *errorMessage,
    const char *detachedErrorMessage);

} // namespace hermes
} // namespace facebook

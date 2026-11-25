/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_STRINGBUILDER_H
#define HERMES_VM_STRINGBUILDER_H

#include "hermes/ADT/SafeInt.h"
#include "hermes/VM/Casting.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StringPrimitive.h"

namespace hermes {
namespace vm {

/// There are many cases we want to allocate a string with a fixed length, and
/// fill in content afterwards.
/// StringBuilder is a helper class that provides such feature.
/// It ensures that the content never exceeds the length, and we can only
/// obtain the string after we have filled in every character.
/// It supports building both ASCII and UTF16 string. The caller states which
/// one to start with: ASCII when every appended piece is known to be ASCII,
/// UTF16 when some piece is known not to be. When the content is not known in
/// advance, starting with ASCII is still correct: appending UTF16 to an ASCII
/// builder makes it create a UTF16 string internally and copy what it has.
/// That copy ignores allocation failure, so a builder that may grow very large
/// and probably needs UTF16 should start with UTF16.
class StringBuilder {
  /// View of the caller-supplied slot that roots the StringPrimitive we are
  /// constructing.
  MutableHandle<StringPrimitive> strPrim_;

  /// Current appending index.
  uint32_t index_;

  /// The runtime. We need this in the rare case when we have to create a new
  /// string when we append UTF16 string into an ASCII string.
  Runtime *runtime_;

 public:
  /// Create a builder for a string of exactly \p length code units, stored as
  /// ASCII if \p isASCII is true and as UTF16 otherwise.
  /// \param storage the caller's rooted slot (e.g. a PinnedValue in its
  ///   Locals) that holds the string under construction. It must outlive the
  ///   builder, and the string it holds is replaced if the builder has to
  ///   switch from ASCII to UTF16.
  static CallResult<StringBuilder> createStringBuilder(
      Runtime &runtime,
      MutableHandle<StringPrimitive> storage,
      SafeUInt32 length,
      bool isASCII) {
    if (length.isOverflowed()) {
      return runtime.raiseRangeError("String length exceeds limit");
    }
    auto crtRes = StringPrimitive::create(runtime, *length, isASCII);
    if (LLVM_UNLIKELY(crtRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return StringBuilder(runtime, storage, crtRes->getString());
  }

  /// Number of characters accumulated in the StringBuilder so far.
  uint32_t currentLength() const {
    return index_;
  }

  /// The length this StringBuilder was constructed with. The max length doesn't
  /// ever change.
  uint32_t maxLength() const {
    return strPrim_->getStringLength();
  }

  /// Append an UTF16Ref \p str. Note that str should not point to a GC-managed
  /// memory, as this function in theory can allocate.
  void appendUTF16Ref(UTF16Ref str) {
    assert(
        index_ + str.size() <= strPrim_->getStringLength() &&
        "StringBuilder append out of bound");
    if (LLVM_UNLIKELY((*strPrim_)->isASCII())) {
      // If we are appending a UTF16 string to an ASCII, we have to recreate
      // the string. This can cause performance issues if misused.
      // The allocation can fail in theory, but in practice, since we are
      // dropping one string and replace it with another, this should be safe.
      auto strRes = runtime_->ignoreAllocationFailure(
          StringPrimitive::create(
              *runtime_, strPrim_->getStringLength(), /*asciiNotUTF16*/ false));
      auto currentPartialString =
          ASCIIRef(strPrim_->castToASCIIPointer(), index_);
      strPrim_ = strRes.getString();
      index_ = 0;
      appendASCIIRef(currentPartialString);
    }
    std::copy(
        str.data(),
        str.data() + str.size(),
        strPrim_->castToUTF16PointerForWrite() + index_);
    index_ += str.size();
  }

  void appendASCIIRef(ASCIIRef ascii) {
    assert(
        index_ + ascii.size() <= strPrim_->getStringLength() &&
        "StringBuilder append out of bound");
    if (LLVM_LIKELY(strPrim_->isASCII())) {
      std::copy(
          ascii.data(),
          ascii.data() + ascii.size(),
          strPrim_->castToASCIIPointerForWrite() + index_);
    } else {
      std::copy(
          ascii.data(),
          ascii.data() + ascii.size(),
          strPrim_->castToUTF16PointerForWrite() + index_);
    }
    index_ += ascii.size();
  }

  /// Append a char16_t character \p ch.
  void appendCharacter(char16_t ch) {
    assert(
        index_ + 1 <= strPrim_->getStringLength() &&
        "StringBuilder append out of bound");
    if (strPrim_->isASCII()) {
      if (ch < 128) {
        strPrim_->castToASCIIPointerForWrite()[index_++] = ch;
      } else {
        // Reuse the implementation of appendUTF16Ref, which will trigger
        // allocating a new UTF16 string.
        appendUTF16Ref(ch);
      }
    } else {
      strPrim_->castToUTF16PointerForWrite()[index_++] = ch;
    }
  }

  /// Append the first \p length characters from StringPrimitive \p other.
  void appendStringPrim(Handle<StringPrimitive> other, uint32_t length) {
    assert(
        index_ + length <= strPrim_->getStringLength() &&
        "StringBuilder append out of bound");
    if (other->isASCII()) {
      appendASCIIRef({other->castToASCIIPointer(), length});
    } else if (!strPrim_->isASCII()) {
      appendUTF16Ref({other->castToUTF16Pointer(), length});
    } else {
      // strPrim_ is ASCII, while other is UTF16. We have to recreate string.
      auto strRes = runtime_->ignoreAllocationFailure(
          StringPrimitive::create(
              *runtime_, strPrim_->getStringLength(), /*asciiNotUTF16*/ false));
      auto currentPartialString =
          ASCIIRef(strPrim_->castToASCIIPointer(), index_);
      // Set strPrim_ to the newly created string.
      strPrim_ = strRes.getString();
      index_ = 0;
      // Append original string and other.
      appendASCIIRef(currentPartialString);
      appendUTF16Ref({other->castToUTF16Pointer(), length});
    }
  }

  /// Append all characters from StringPrimitive \p other.
  void appendStringPrim(Handle<StringPrimitive> other) {
    return appendStringPrim(other, other->getStringLength());
  }

  /// After appending finished, return the StringPrimitive. The handle refers to
  /// the storage slot passed to createStringBuilder.
  Handle<StringPrimitive> getStringPrimitive() const {
    assert(
        index_ == strPrim_->getStringLength() &&
        "String content is not fully filled yet.");
    return Handle<StringPrimitive>::vmcast(strPrim_);
  }

 private:
  StringBuilder(
      Runtime &runtime,
      MutableHandle<StringPrimitive> storage,
      StringPrimitive *strPrim)
      : strPrim_(storage), index_(0), runtime_(&runtime) {
    strPrim_ = strPrim;
  }
};

} // namespace vm
} // namespace hermes

#endif

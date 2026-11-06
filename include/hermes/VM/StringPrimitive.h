/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_STRINGPRIMITIVE_H
#define HERMES_VM_STRINGPRIMITIVE_H

#include "hermes/Support/CheckedMalloc.h"
#include "hermes/Support/ErrorHandling.h"
#include "hermes/VM/CopyableBasicString.h"
#include "hermes/VM/GC.h"
#include "hermes/VM/GCBase.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StringRefUtils.h"

#include "llvh/Support/TrailingObjects.h"

#include <type_traits>

namespace hermes {
namespace vm {

class StringView;

/// The base class for all types of StringPrimitives. In most of the cases,
/// use of strings should not need to worry about the precise type of
/// StringPrimitive, and use it directly in favor of the exact subclass.
class StringPrimitive : public VariableSizeRuntimeCell {
  friend RuntimeOffsets;

 protected:
  // Fields:

  /// Length of the string in 16-bit characters.
  /// The highest bit is set to 1 if the string has been uniqued.
  /// The second highest bit is set to 1 if the hash has been cached.
  uint32_t lengthAndFlags_;

  /// Cached hashString hash of the whole StringPrimitive.
  /// Only valid if the LENGTH_FLAG_HASH bit in lengthAndFlags_ is set.
  uint32_t hash_;

  // Constants:

  /// Flag set in the \c lengthAndFlags_ field to indicate that this
  /// string was "uniqued", that is, inserted into the identifier hash table and
  /// the associated SymbolID was stored in the string.
  /// Not all StringPrimitive subclasses support this and it usually happens on
  /// construction.
  /// This flag is automatically cleared by the identifier table when the
  /// associated SymbolID is garbage collected.
  static constexpr uint32_t LENGTH_FLAG_UNIQUED = uint32_t(1) << 31;

  /// Flag set in the \c lengthAndFlags_ field to indicate that this string's
  /// hash has been cached in the hash_ field.
  static constexpr uint32_t LENGTH_FLAG_HASH = uint32_t(1) << 30;

  /// Bits used for the length in \c lengthAndFlags_.
  static constexpr uint32_t LENGTH_MASK =
      ~(LENGTH_FLAG_UNIQUED | LENGTH_FLAG_HASH);

 private:
  friend class detail::IdentifierHashTable;
  friend class IdentifierTable;
  friend class StringBuilder;
  friend class StringView;
  template <typename T>
  friend class BufferedStringPrimitive;

  friend llvh::raw_ostream &operator<<(
      llvh::raw_ostream &OS,
      const StringPrimitive *str);

  /// Private implementation of createEfficient.
  /// Create a StringPrimitive containing the contents of \p str.
  /// If \p optStorage is provided, the implementation may acquire ownership of
  /// it.
  template <typename T>
  static CallResult<HermesValue> createEfficientImpl(
      Runtime &runtime,
      llvh::ArrayRef<T> str,
      std::basic_string<T> *optStorage = nullptr);

  /// Create a new DynamicASCIIStringPrimitive if str is all ASCII, otherwise
  /// create a new DynamicUTF16StringPrimitive.
  static CallResult<HermesValue> createDynamic(Runtime &runtime, UTF16Ref str);

  /// Create a new DynamicASCIIStringPrimitive if \param isASCII is true,
  /// otherwise create a new DynamicUTF16StringPrimitive.
  static CallResult<HermesValue>
  createDynamicWithKnownEncoding(Runtime &runtime, UTF16Ref str, bool isASCII);

  /// Create an ExternalStringPrimitive with the contents of \p str, which must
  /// have an external length: ASCII if \p isASCII is true, otherwise UTF16.
  /// Raises a RangeError if the GC cannot admit the external memory.
  static CallResult<HermesValue>
  createExternal(Runtime &runtime, UTF16Ref str, bool isASCII);

  /// Like the above, storing \p str as ASCII if every code unit is ASCII.
  static CallResult<HermesValue> createExternal(Runtime &runtime, UTF16Ref str);

  /// The following private overloads are to prevent creation of a
  /// StringPrimitive from a string literal. Naively allowing this would invoke
  /// ArrayRef's array template constructor, which would include the terminating
  /// NUL (as string literals are arrays that include the NUL). Use instead
  /// create() variants that are explicit about the length, e.g. ASCIIRef or
  /// UTF16Ref parameters.
  template <typename CharT, size_t N>
  static CallResult<HermesValue> create(Runtime &, const CharT (&Arr)[N]);

  /// As create(), but overloading createLongLived().
  template <typename CharT, size_t N>
  static CallResult<HermesValue> createLongLived(
      Runtime &,
      const CharT (&Arr)[N]);

  /// Internal helper to return a std::string from an ArrayRef.
  template <typename CharT>
  static std::basic_string<CharT> arrayToString(llvh::ArrayRef<CharT> arr) {
    return std::basic_string<CharT>{arr.begin(), arr.end()};
  }

 protected:
  /// Super constructor to set the length properly.
  explicit StringPrimitive(uint32_t length) : lengthAndFlags_(length) {}

  /// Returns true if a string of the given \p length should be allocated as an
  /// external string, outside the JS heap. Note that some external strings may
  /// be shorter than this length. \p length is a size_t so that the length of
  /// an input buffer is compared without truncation.
  static bool isExternalLength(size_t length) {
    return length >= EXTERNAL_STRING_THRESHOLD;
  }

  /// Returns true if a string is safe to allocate externally (strings that are
  /// too small are not safe).
  static bool isSafeExternalLength(uint32_t length) {
    return length >= EXTERNAL_STRING_MIN_SIZE;
  }

 public:
  /// No string with length exceeding this constant can be allocated.
  static constexpr uint32_t MAX_STRING_LENGTH = 256 * 1024 * 1024;

  /// Strings whose length is at least this size are always allocated as
  /// "external" strings, outside the JS heap. Note that there may be external
  /// strings smaller than this length.
  /// This is literally 1/64 of the unit segment size, so that in builds with
  /// small segment size, we won't allocate a too large string in the heap.
  static constexpr uint32_t EXTERNAL_STRING_THRESHOLD =
      std::min<size_t>(AlignedHeapSegment::kSegmentUnitSize >> 6, 64 * 1024);

  /// Strings whose length is smaller than this will never be externally
  /// allocated. This is to protect against a small string optimization which
  /// may use interior pointers, which would break when memcpy'd by the GC. This
  /// is also the size at which StringPrimitive will acquire an std::string via
  /// ownership transfer. This is advantageous in that it reduces the amount of
  /// copying from the malloc heap to the GC heap. However it should not be too
  /// small, because the std::string itself imposes a space overhead.
  static constexpr uint32_t EXTERNAL_STRING_MIN_SIZE =
      COPYABLE_BASIC_STRING_MIN_LENGTH;

  /// Concatenation resulting in this size or larger will use
  /// BufferedStringPrimitive. We want to ensure that they satisfy the
  /// requirements for external strings.
  static constexpr uint32_t CONCAT_STRING_MIN_SIZE =
      std::max(256u, EXTERNAL_STRING_MIN_SIZE);

  static bool classof(const GCCell *cell) {
    return kindInRange(
        cell->getKind(),
        CellKind::StringPrimitiveKind_first,
        CellKind::StringPrimitiveKind_last);
  }

  /// Proxy to {Dynamic,External}StringPrimitive<char>::create(runtime, length).
  /// The \p asciiNotUTF16 argument indicates whether the character type is char
  /// (if true) or char16_t (if false).
  static CallResult<HermesValue>
  create(Runtime &runtime, uint32_t length, bool asciiNotUTF16);

  /// Proxy to {Dynamic,External}StringPrimitive<char>::create(runtime, str).
  static CallResult<HermesValue> create(Runtime &runtime, ASCIIRef str);

  /// Create a string with the contents of \p str, stored as ASCII if every
  /// code unit is ASCII and as UTF16 otherwise.
  static CallResult<HermesValue> create(Runtime &runtime, UTF16Ref str);

  /// Like create(Runtime &, UTF16Ref), with the caller stating whether every
  /// code unit of \p str is ASCII in \p isASCII.
  static CallResult<HermesValue>
  createWithKnownEncoding(Runtime &runtime, UTF16Ref str, bool isASCII);

  /// Create a StringPrimitive as efficiently as the contents of \p str allow.
  /// If it is the empty string or a single character string,
  /// return a string interned in the Runtime.
  static CallResult<HermesValue> createEfficient(
      Runtime &runtime,
      ASCIIRef str);

  static CallResult<HermesValue> createEfficient(
      Runtime &runtime,
      UTF16Ref str);

  /// Create a StringPrimitive as efficiently as the contents of \p str allow.
  /// UTF8 inputs will be converted to UTF16. Invalid code points in \p str will
  /// cause a RangeError (if \p IgnoreInputErrors is false) to be raised, or a
  /// truncated PrimitiveString (if \p IgnoreInputErrors is true) to be returned
  /// -- its contents will stop right before the first invalid code point in
  /// \p str.
  static CallResult<HermesValue> createEfficient(
      Runtime &runtime,
      UTF8Ref str,
      bool IgnoreInputErrors = false);

  /// Versions of createEfficient that allow for ownership transfer of an
  /// std::string. Create a StringPrimitive from \p str. The implementation may
  /// choose to acquire ownership of \p str and use it to back the string.
  static CallResult<HermesValue> createEfficient(
      Runtime &runtime,
      std::basic_string<char> &&str);

  static CallResult<HermesValue> createEfficient(
      Runtime &runtime,
      std::basic_string<char16_t> &&str);

  /// Like the above, but the created StringPrimitives will be
  /// allocated in a "long-lived" area of the heap (if the GC supports
  /// that concept).
  static CallResult<HermesValue> createLongLived(
      Runtime &runtime,
      ASCIIRef str);
  static CallResult<HermesValue> createLongLived(
      Runtime &runtime,
      UTF16Ref str);

  /// Copy a UTF-16 sequence into a new StringPrim without throwing.
  /// This function should only be used during VM initialization, where OOM
  /// should never happen.
  static inline Handle<StringPrimitive> createNoThrow(
      Runtime &runtime,
      UTF16Ref str);

  /// Copy a UTF-16 sequence into a new StringPrim without throwing.
  /// This function should only be used during VM initialization, where OOM
  /// should never happen.
  static inline Handle<StringPrimitive> createNoThrow(
      Runtime &runtime,
      llvh::StringRef ascii);

  /// \return the length of string in 16-bit characters.
  uint32_t getStringLength() const {
    return lengthAndFlags_ & LENGTH_MASK;
  }

  /// \return whether the string is uniqued.
  bool isUniqued() const {
    return (lengthAndFlags_ & LENGTH_FLAG_UNIQUED) != 0;
  }

  /// Compare a part of this string to \p other for equality.
  /// \return true if the section of this string from \p start of length \p
  /// length is equal to the string \p other.
  bool sliceEquals(
      uint32_t start,
      uint32_t length,
      const StringPrimitive *other) const;

  /// \return true if the other string is identical to this one.
  bool equals(const StringPrimitive *other) const {
    if (this == other)
      return true;
    if (getStringLength() != other->getStringLength())
      return false;
    return sliceEquals(0, getStringLength(), other);
  }

  /// \return true if the other string view has identical content as self.
  bool equals(const StringView &other) const;

  /// Lexicographically compare the two strings.
  /// \return -1 if `this` is smaller, 0 if equal, +1 if `this` is greater.
  int compare(const StringPrimitive *other) const;

  /// \return the JenkinsHash hash of this string.
  /// If it's not cached already, compute it and cache it.
  uint32_t getOrComputeHash() {
    if ((lengthAndFlags_ & LENGTH_FLAG_HASH) == 0)
      setHash(computeHash());
    return hash_;
  }

  /// When the hash is known by the user, allow the user to set it.
  /// Doesn't allow the user to specify a different hash.
  /// This simply can be used to avoid recomputing it in certain cases.
  /// \param hash must be the correct hash which would have been computed by
  ///   getOrComputeHash().
  void setHash(uint32_t hash) {
    assert((lengthAndFlags_ & LENGTH_FLAG_HASH) == 0 && "hash set twice");
    assert(hash == computeHash() && "incorrect hash provided");
    hash_ = hash;
    lengthAndFlags_ |= LENGTH_FLAG_HASH;
  }

  /// Concatenate two StringPrimitives at \p xHandle and \p yHandle.
  /// \return pointer to a new StringPrimitive, representing the concatenation.
  static CallResult<HermesValue> concat(
      Runtime &runtime,
      Handle<StringPrimitive> xHandle,
      Handle<StringPrimitive> yHandle);

  /// Slice the StringPrimitive at \p str, \p length characters at \p start.
  /// \return new StringPrimitive, representing the sliced string.
  static CallResult<HermesValue> slice(
      Runtime &runtime,
      Handle<StringPrimitive> str,
      size_t start,
      size_t length);

  /// Flatten the string if it's a rope, possibly causing allocation/GC.
  static Handle<StringPrimitive> ensureFlat(
      Runtime &runtime,
      Handle<StringPrimitive> self) {
    // In the future, ensureFlat may trigger GC as it might allocate for
    // ropes. Move the heap here.
    runtime.potentiallyMoveHeap();
    return self;
    // TODO: Deal with different subclasses (e.g. rope)
  }

  /// \return true if the string is flat.
  bool isFlat() const {
    return true;
  }

  /// \return a StringView of this string. In the case of a rope, we will need
  /// to resolve the rope, which might involve object allocations.
  static StringView createStringView(
      Runtime &runtime,
      Handle<StringPrimitive> self);

  /// In the rare case (most likely for debugging, printing and etc), we just
  /// want to get a copy of the string in UTF16 form, without worrying about
  /// performance and efficiency. The string will be copied into \p str.
  void appendUTF16String(llvh::SmallVectorImpl<char16_t> &str) const;

  /// \return the character at \p index.
  /// Use it only when you cannot use a StringView.
  inline char16_t at(uint32_t index) const;

  /// Whether this is an ASCII string.
  inline bool isASCII() const;

  /// Whether this is an external string.
  inline bool isExternal() const;

  /// Get a StringRef of T. T must be char or char16_t corresponding to whether
  /// this string is ASCII or UTF-16.
  template <typename T>
  llvh::ArrayRef<T> getStringRef() const {
    return llvh::ArrayRef<T>{castToPointer<T>(), getStringLength()};
  }

  /// Get a writable raw char pointer, assert that this is ASCII string.
  char *castToASCIIPointerForWrite();

  /// Get a writable raw char16_t pointer, assert that this is UTF16 string.
  char16_t *castToUTF16PointerForWrite();

 private:
  /// Similar to appendUTF16String(SmallVectorImpl), copy the string into
  /// a raw pointer \p ptr. Since there is no size check, this function should
  /// only be called in rare cases carefully.
  void appendUTF16String(char16_t *ptr) const;

  /// Get a read-only raw char pointer, assert that this is ASCII string.
  const char *castToASCIIPointer() const;

  /// Get a read-only raw char16_t pointer, assert that this is UTF16 string.
  const char16_t *castToUTF16Pointer() const;

  /// Get a read-only pointer to char or char16_t. Assert this is a string of
  /// the correct type.
  template <typename T>
  inline const T *castToPointer() const;

  /// \return the UTF-16 ref starting at \p start of length \p length.
  UTF16Ref castToUTF16Ref(uint32_t start, uint32_t length) const {
    return UTF16Ref(castToUTF16Pointer() + start, length);
  }

  /// \return the ASCII ref starting at \p start of length \p length.
  ASCIIRef castToASCIIRef(uint32_t start, uint32_t length) const {
    return ASCIIRef(castToASCIIPointer() + start, length);
  }

  UTF16Ref castToUTF16Ref() const {
    return castToUTF16Ref(0, getStringLength());
  }

  ASCIIRef castToASCIIRef() const {
    return castToASCIIRef(0, getStringLength());
  }

  /// \return the JenkinsHash of this string.
  uint32_t computeHash() const;

  /// In cases when we know the String cannot be a rope (e.g. as Identifier),
  /// it is safe to call this function which guarantees to not trigger gc.
  static StringView createStringViewMustBeFlat(Handle<StringPrimitive> self);

 protected:
  /// \return whether the StringPrimitive can be converted from non-uniqued to
  /// uniqued without reallocating.
  inline bool canBeUniqued() const;

  /// Convert a non-uniqued StringPrimitive to a unique one.
  /// \pre \c canBeUniqued() returns \c true.
  inline void convertToUniqued(SymbolID uniqueID);

  /// \return the unique id.
  /// This requires and asserts that the string is uniqued.
  SymbolID getUniqueID(Runtime &runtime) const;

  /// Mark this string as not uniqued. This is used by IdentifierTable when
  /// the associated SymbolID is garbage collected.
  void clearUniquedBit() {
    lengthAndFlags_ &= ~LENGTH_FLAG_UNIQUED;
  }

#ifdef HERMES_MEMORY_INSTRUMENTATION
  static std::string _snapshotNameImpl(GCCell *cell, GC &gc);
#endif
};

/// A subclass of StringPrimitive which stores a SymbolID.
/// Note that not all SymbolStringPrimitive are uniqued.
/// All ExternalStringPrimitives store a Symbol, but only those marked as
/// uniqued will use it.
class SymbolStringPrimitive : public StringPrimitive {
  /// The SymbolID that was assigned to this StringPrimitive when it was
  /// "uniqued" - inserted into the identifier table.
  /// This field is *only* valid if \c isUniqued() returns \c true. Otherwise
  /// its value must be ignored (even though the field itself might not be
  /// cleared).
  /// If the associated SymbolID is garbage collected, the uniqued flag will
  /// be cleared and this field becomes invalid (but won't actually be cleared,
  /// for performance reasons).
  SymbolID weakUniqueID_{};

 public:
  using StringPrimitive::StringPrimitive;

  static bool classof(const GCCell *cell) {
    static_assert(
        cellKindsContiguousAscending(
            CellKind::DynamicUniquedUTF16StringPrimitiveKind,
            CellKind::DynamicUniquedASCIIStringPrimitiveKind,
            CellKind::ExternalUTF16StringPrimitiveKind,
            CellKind::ExternalASCIIStringPrimitiveKind),
        "Unexpected CellKind ordering");
    return kindInRange(
        cell->getKind(),
        CellKind::DynamicUniquedUTF16StringPrimitiveKind,
        CellKind::ExternalASCIIStringPrimitiveKind);
  }

 protected:
  friend class StringPrimitive;

  /// Set the unique id. This should normally only be done immediately after
  /// construction.
  void updateUniqueID(SymbolID id) {
    assert(isUniqued() && "StringPrimitive is not uniqued");
    weakUniqueID_ = id;
  }

  /// \return the unique id.
  SymbolID getUniqueID(Runtime &runtime) const {
    assert(isUniqued() && "StringPrimitive is not uniqued");
    runtime.getHeap().weakRefReadBarrier(weakUniqueID_);
    return weakUniqueID_;
  }
};

/// A helper typedef which is either SymbolStringPrimitive or StringPrimitive.
template <bool Uniqued>
using OptSymbolStringPrimitive = typename std::
    conditional<Uniqued, SymbolStringPrimitive, StringPrimitive>::type;

/// An immutable JavaScript primitive string consisting of length and
/// characters (either char or char16), optionally Uniqued.
/// The storage is allocated in and managed by the GC.
template <typename T, bool Uniqued>
class DynamicStringPrimitive final
    : public OptSymbolStringPrimitive<Uniqued>,
      private llvh::TrailingObjects<DynamicStringPrimitive<T, Uniqued>, T> {
  friend class IdentifierTable;
  friend class llvh::TrailingObjects<DynamicStringPrimitive<T, Uniqued>, T>;
  friend class StringBuilder;
  friend class StringPrimitive;
  friend void DynamicASCIIStringPrimitiveBuildMeta(
      const GCCell *,
      Metadata::Builder &);
  friend void DynamicUTF16StringPrimitiveBuildMeta(
      const GCCell *,
      Metadata::Builder &);
  friend void DynamicUniquedASCIIStringPrimitiveBuildMeta(
      const GCCell *,
      Metadata::Builder &);
  friend void DynamicUniquedUTF16StringPrimitiveBuildMeta(
      const GCCell *,
      Metadata::Builder &);
  using OptSymbolStringPrimitive<Uniqued>::isExternalLength;
  using OptSymbolStringPrimitive<Uniqued>::getStringLength;

  using Ref = llvh::ArrayRef<T>;

 public:
  /// \return the cell kind for this string.
  static constexpr CellKind getCellKind() {
    return std::is_same<T, char16_t>::value
        ? (Uniqued ? CellKind::DynamicUniquedUTF16StringPrimitiveKind
                   : CellKind::DynamicUTF16StringPrimitiveKind)
        : (Uniqued ? CellKind::DynamicUniquedASCIIStringPrimitiveKind
                   : CellKind::DynamicASCIIStringPrimitiveKind);
  }

  static bool classof(const GCCell *cell) {
    return cell->getKind() == DynamicStringPrimitive::getCellKind();
  }

 private:
  static const VTable vt;

 public:
  /// Construct from a DynamicStringPrimitive, perhaps with a SymbolID.
  /// If a non-empty SymbolID is provided, we must be a Uniqued string.
  explicit DynamicStringPrimitive(uint32_t length)
      : OptSymbolStringPrimitive<Uniqued>(length) {
    assert(!isExternalLength(length) && "length should not be external");
  }

  explicit DynamicStringPrimitive(Ref src);

 private:
  /// Copy a UTF-16 sequence into a new StringPrim. Throw \c RangeError if the
  /// string is longer than \c MAX_STRING_LENGTH characters. The new string is
  static CallResult<HermesValue> create(Runtime &runtime, Ref str);

  /// Like the above, but the created StringPrimitive will be
  /// allocated in a "long-lived" area of the heap (if the GC supports
  /// that concept).
  static CallResult<HermesValue> createLongLived(Runtime &runtime, Ref str);

  /// Create a StringPrim object with a specified capacity \p length in
  /// 16-bit characters. Throw \c RangeError if the string is longer than
  /// \c MAX_STRING_LENGTH characters. The new string is returned in
  /// \c CallResult<HermesValue>. This should only be used by StringBuilder.
  static CallResult<HermesValue> create(Runtime &runtime, uint32_t length);

  /// Calculate the allocation size of a StringPrimitive given character
  /// length.
  static uint32_t allocationSize(uint32_t length) {
    // In the future it would be better to have the GC return a new size
    // instead of having the caller decide.
    return std::max(
        DynamicStringPrimitive::template totalSizeToAlloc<T>(length),
        static_cast<size_t>(GC::minAllocationSize()));
  }

  const T *getRawPointer() const {
    return this->template getTrailingObjects<T>();
  }

  /// In rare cases it is convenient to allocate an object without populating
  /// the string contents and then initialize it dynamically. It should not
  /// normally be done, but for those rare cases, this method gives access to
  /// the writable buffer.
  T *getRawPointerForWrite() {
    return this->template getTrailingObjects<T>();
  }
};

/// An immutable JavaScript primitive string consisting of length and a pointer
/// to characters (either char or char16). The storage uses std::string or
/// std::u16string, and the object's finalizer deallocates the storage.
/// Note: while StringPrimitive extends VariableSizeRuntimeCell, these subtypes
/// are not actually variable-sized: we indicate that they are fixed-size in the
/// metadata.
template <typename T>
class ExternalStringPrimitive final : public SymbolStringPrimitive {
  friend class IdentifierTable;
  friend class StringBuilder;
  friend class StringPrimitive;
  // BufferedStringPrimitive uses this class for storage and needs to be able
  // to append to the contents.
  template <typename U>
  friend class BufferedStringPrimitive;
  friend PseudoHandle<StringPrimitive> internalConcatStringPrimitives(
      Runtime &runtime,
      Handle<StringPrimitive> leftHnd,
      Handle<StringPrimitive> rightHnd);
  friend void ExternalASCIIStringPrimitiveBuildMeta(
      const GCCell *,
      Metadata::Builder &);
  friend void ExternalUTF16StringPrimitiveBuildMeta(
      const GCCell *,
      Metadata::Builder &);

  using Ref = llvh::ArrayRef<T>;
  using StdString = std::basic_string<T>;
  using CopyableStdString = CopyableBasicString<T>;

 public:
  /// \return the cell kind for this string.
  static constexpr CellKind getCellKind() {
    return std::is_same<T, char16_t>::value
        ? CellKind::ExternalUTF16StringPrimitiveKind
        : CellKind::ExternalASCIIStringPrimitiveKind;
  }

  static bool classof(const GCCell *cell) {
    return cell->getKind() == ExternalStringPrimitive::getCellKind();
  }

 private:
  static const VTable vt;

  size_t calcExternalMemorySize() const {
    return contents_.capacity() * sizeof(T);
  }

 public:
  /// Construct an ExternalStringPrimitive from the given string \p contents,
  /// non-uniqued.
  template <class BasicString>
  ExternalStringPrimitive(BasicString &&contents);

 private:
  /// Destructor deallocates the contents_ string.
  ~ExternalStringPrimitive() = default;

  /// Transfer ownership of an std::string into a new StringPrim. Throw \c
  /// RangeError if the string is longer than \c MAX_STRING_LENGTH characters.
  template <class BasicString>
  static CallResult<HermesValue> create(Runtime &runtime, BasicString &&str);

  /// Like the above, but the created StringPrimitive will be allocated in a
  /// "long-lived" area of the heap (if the GC supports that concept).  Note
  /// that this applies only to the object proper; the contents array is
  /// allocated outside the JS heap in either case.
  static CallResult<HermesValue> createLongLived(
      Runtime &runtime,
      StdString &&str);

  /// Create a StringPrim object with a specified capacity \p length in
  /// 16-bit characters. Throw \c RangeError if the string is longer than
  /// \c MAX_STRING_LENGTH characters. The new string is returned in
  /// \c CallResult<HermesValue>. This should only be used by StringBuilder.
  static CallResult<HermesValue> create(Runtime &runtime, uint32_t length);

  const T *getRawPointer() const {
    // C++11 defines this to be valid even if the string is empty.
    return &contents_[0];
  }

  /// In rare cases it is convenient to allocate an object without populating
  /// the string contents and then initialize it dynamically. It should not
  /// normally be done, but for those rare cases, this method gives access to
  /// the writable buffer.
  T *getRawPointerForWrite() {
    // C++11 defines this to be valid even if the string is empty.
    return &contents_[0];
  }

  // Finalizer to clean up the malloc'ed string.
  static void _finalizeImpl(GCCell *cell, GC &gc);

  /// \return the size of the external memory associated with \p cell, which is
  /// assumed to be an ExternalStringPrimitive.
  static size_t _mallocSizeImpl(GCCell *cell);

#ifdef HERMES_MEMORY_INSTRUMENTATION
  static void _snapshotAddEdgesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap);
  static void _snapshotAddNodesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap);
#endif

  /// The backing storage of this string. Note that the string's length is fixed
  /// and must always be equal to StringPrimitive::getStringLength().
  CopyableStdString contents_{};
};

/// An immutable JavaScript primitive consisting of a pointer to an
/// ExternalStringPrimitive and a length. This is the result of a concatenation
/// operation, where the referenced ExternalStringPrimitive contains a growing
/// std::string. Each subsequent concatenation appends data to the std::string
/// and allocates a new BufferedStringPrimitive referring to a prefix of it.
///
/// A degenerate case could result from code that keeps a reference only to an
/// early stage in the "concatenation chain", because it would keep the whole
/// large string alive. Something like this:
/// \code
///    globalThis.prop = x + " ";
///    print(globalThis.prop + largeString);
/// \endcode
/// \c globalThis.prop will keep the whole string alive, even though we no
/// longer need the \c largeString suffix.
///
/// The probability of this happening is not high, but it is possible. One way
/// to address this in the future is to create new ExternalStringPrimitive after
/// a  certain amount of resizing. In this way each stage of the concatenation
/// has an upper bound of the amount of extra memory it can keep alive.
template <typename T>
class BufferedStringPrimitive final : public StringPrimitive {
  friend class IdentifierTable;
  friend class StringBuilder;
  friend class StringPrimitive;
  friend PseudoHandle<StringPrimitive> internalConcatStringPrimitives(
      Runtime &runtime,
      Handle<StringPrimitive> leftHnd,
      Handle<StringPrimitive> rightHnd);
  friend void BufferedASCIIStringPrimitiveBuildMeta(
      const GCCell *cell,
      Metadata::Builder &mb);
  friend void BufferedUTF16StringPrimitiveBuildMeta(
      const GCCell *cell,
      Metadata::Builder &mb);

  using Ref = llvh::ArrayRef<T>;
  using StdString = std::basic_string<T>;

 public:
  /// \return the cell kind for this string.
  static constexpr CellKind getCellKind() {
    return std::is_same<T, char16_t>::value
        ? CellKind::BufferedUTF16StringPrimitiveKind
        : CellKind::BufferedASCIIStringPrimitiveKind;
  }

  static bool classof(const GCCell *cell) {
    return cell->getKind() == BufferedStringPrimitive::getCellKind();
  }

#ifdef UNIT_TEST
  /// Expose the concatenation buffer for unit tests.
  ExternalStringPrimitive<T> *testGetConcatBuffer() const {
    return vmcast<ExternalStringPrimitive<T>>(concatBufferHV_);
  }
#endif

 private:
  static const VTable vt;

 public:
  /// Construct a BufferedStringPrimitive with the specified length \p length
  /// and the associated concatenation buffer \p storage. Note that the length
  /// of the primitive may be smaller than the length of the buffer.
  BufferedStringPrimitive(
      Runtime &runtime,
      uint32_t length,
      Handle<ExternalStringPrimitive<T>> concatBuffer)
      : StringPrimitive(length) {
    concatBufferHV_.set(concatBuffer.getHermesValue(), runtime.getHeap());
    assert(
        concatBuffer->contents_.size() >= length &&
        "length exceeds size of concatenation buffer");
  }

 private:
  /// Allocate a BufferedStringPrimitive with the specified length \p length
  /// and the associated concatenation buffer \p storage. Note that the length
  /// of the primitive may be smaller than the length of the buffer.
  static PseudoHandle<StringPrimitive> create(
      Runtime &runtime,
      uint32_t length,
      Handle<ExternalStringPrimitive<T>> storage);

  /// Append a new string to the concatenation buffer and allocate a new
  /// BufferedStringPrimitive representing the result.
  /// \pre The types must be compatible (cannot append UTF16 to ASCII) and the
  /// combined length must have been validated.
  /// \return the new BufferedStringPrimitive representing the result.
  static PseudoHandle<StringPrimitive> append(
      Handle<BufferedStringPrimitive<T>> selfHnd,
      Runtime &runtime,
      Handle<StringPrimitive> rightHnd);

  /// Create a new concatenation buffer of type T (the type parameter of this
  /// template classes), initialize it with the concatenation of \p leftHnd and
  /// \p rightHnd and allocate a new BufferedStringPrimitive to represent the
  /// result.
  /// \pre The types must be compatible with respect to T (cannot append UTF16
  /// to ASCII) and the combined length must have been validated.
  /// \return a new BufferedStringPrimitive representing the result.
  static PseudoHandle<StringPrimitive> create(
      Runtime &runtime,
      Handle<StringPrimitive> leftHnd,
      Handle<StringPrimitive> rightHnd);

  /// Append a string primitive to the StdString \p res, performing an ASCII to
  /// UTF16 conversion if necessary.
  /// \pre cannot append UTF16 to ASCII.
  static void appendToCopyableString(
      CopyableBasicString<T> &res,
      const StringPrimitive *str);

  /// \return a const pointer to the first character of the string.
  const T *getRawPointer() const {
    return getConcatBuffer()->getRawPointer();
  }

  /// A helper to cast \c concatBufferHV_ to a typed pointer to
  /// \c ExternalStringPrimitive.
  /// \return the ExternalStringPrimitive used as a concatenation buffer.
  ExternalStringPrimitive<T> *getConcatBuffer() const {
    return vmcast<ExternalStringPrimitive<T>>(concatBufferHV_);
  }

#ifdef HERMES_MEMORY_INSTRUMENTATION
  static void _snapshotAddEdgesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap);
  static void _snapshotAddNodesImpl(GCCell *cell, GC &gc, HeapSnapshot &snap);
#endif

  /// Reference to an ExternalStringPrimitive used as a concatenation buffer.
  /// Every concatenation appends to it and allocates a new
  /// BufferedStringPrimitive.
  /// For now we are using a GCHermesValue instead of GCPointer to avoid
  /// refactoring around compressed pointers, which require PointerBase to be
  /// passed to functions which didn't previously need it.
  GCHermesValue concatBufferHV_;
};

/// \return true if this is one of the BufferedStringPrimitive classes.
inline bool isBufferedStringPrimitive(const GCCell *cell) {
  return cell->getKind() == CellKind::BufferedUTF16StringPrimitiveKind ||
      cell->getKind() == CellKind::BufferedASCIIStringPrimitiveKind;
}

/// This function is not part of the API and is not supposed to be called
/// directly. It is used internally by StringPrimitive::concat. It is used
/// to handle the case when the result string exceeds the minimal length for
/// buffered concatenation, or when the left string is already a
/// BufferedStringPrimitive. Internally it does the right thing by either
/// appending to an existing concatenation buffer, if it can, or by allocating a
/// new one.
/// Some cases where it needs to allocate a new buffer include:
/// - the left string is not a BufferedStringPrimitive
/// - appending UTF16 to ASCII
/// - appending to the middle of the concatenation chain.
/// \pre The combined length must have been validated by the caller.
PseudoHandle<StringPrimitive> internalConcatStringPrimitives(
    Runtime &runtime,
    Handle<StringPrimitive> leftHnd,
    Handle<StringPrimitive> rightHnd);

template <typename T, bool Uniqued>
const VTable DynamicStringPrimitive<T, Uniqued>::vt = VTable(
    DynamicStringPrimitive<T, Uniqued>::getCellKind(),
    0,
    /* allowLargeAlloc */ false,
    nullptr,
    nullptr,
    nullptr
#ifdef HERMES_MEMORY_INSTRUMENTATION
    ,
    VTable::HeapSnapshotMetadata{
        HeapSnapshot::NodeType::String,
        DynamicStringPrimitive<T, Uniqued>::_snapshotNameImpl,
        nullptr,
        nullptr,
        nullptr}
#endif
);

using DynamicUTF16StringPrimitive =
    DynamicStringPrimitive<char16_t, false /* not Uniqued */>;
using DynamicASCIIStringPrimitive =
    DynamicStringPrimitive<char, false /* not Uniqued */>;
using DynamicUniquedUTF16StringPrimitive =
    DynamicStringPrimitive<char16_t, true /* Uniqued */>;
using DynamicUniquedASCIIStringPrimitive =
    DynamicStringPrimitive<char, true /* Uniqued */>;

template <typename T>
const VTable ExternalStringPrimitive<T>::vt = VTable(
    ExternalStringPrimitive<T>::getCellKind(),
    0,
    /* allowLargeAlloc */ false,
    ExternalStringPrimitive<T>::_finalizeImpl,
    ExternalStringPrimitive<T>::_mallocSizeImpl,
    nullptr
#ifdef HERMES_MEMORY_INSTRUMENTATION
    ,
    VTable::HeapSnapshotMetadata{
        HeapSnapshot::NodeType::String,
        ExternalStringPrimitive<T>::_snapshotNameImpl,
        ExternalStringPrimitive<T>::_snapshotAddEdgesImpl,
        ExternalStringPrimitive<T>::_snapshotAddNodesImpl,
        nullptr}
#endif
);

using ExternalUTF16StringPrimitive = ExternalStringPrimitive<char16_t>;
using ExternalASCIIStringPrimitive = ExternalStringPrimitive<char>;

template <typename T>
const VTable BufferedStringPrimitive<T>::vt = VTable(
    BufferedStringPrimitive<T>::getCellKind(),
    0,
    /* allowLargeAlloc */ false,
    nullptr, // finalize.
    nullptr, // mallocSize
    nullptr
#ifdef HERMES_MEMORY_INSTRUMENTATION
    ,
    VTable::HeapSnapshotMetadata{
        HeapSnapshot::NodeType::String,
        BufferedStringPrimitive<T>::_snapshotNameImpl,
        BufferedStringPrimitive<T>::_snapshotAddEdgesImpl,
        BufferedStringPrimitive<T>::_snapshotAddNodesImpl,
        nullptr}
#endif
);

using BufferedUTF16StringPrimitive = BufferedStringPrimitive<char16_t>;
using BufferedASCIIStringPrimitive = BufferedStringPrimitive<char>;

//===----------------------------------------------------------------------===//
// StringPrimitive inline methods.

inline llvh::raw_ostream &operator<<(
    llvh::raw_ostream &OS,
    const StringPrimitive *str) {
  if (str->isASCII()) {
    return OS << str->castToASCIIRef();
  }
  return OS << str->castToUTF16Ref();
}

/*static*/ inline Handle<StringPrimitive> StringPrimitive::createNoThrow(
    Runtime &runtime,
    UTF16Ref str) {
  auto strRes = create(runtime, str);
  if (strRes == ExecutionStatus::EXCEPTION) {
    hermes_fatal("String allocation failed");
  }
  return runtime.makeHandle<StringPrimitive>(*strRes);
}

/*static*/ inline Handle<StringPrimitive> StringPrimitive::createNoThrow(
    Runtime &runtime,
    llvh::StringRef ascii) {
  auto strRes = create(runtime, ASCIIRef(ascii.data(), ascii.size()));
  if (strRes == ExecutionStatus::EXCEPTION) {
    hermes_fatal("String allocation failed");
  }
  return runtime.makeHandle<StringPrimitive>(*strRes);
}

inline CallResult<HermesValue>
StringPrimitive::create(Runtime &runtime, uint32_t length, bool asciiNotUTF16) {
  static_assert(
      EXTERNAL_STRING_THRESHOLD < MAX_STRING_LENGTH,
      "External string threshold should be smaller than max string size.");
  static_assert(
      MAX_STRING_LENGTH <= LENGTH_MASK,
      "Highest bits of length are reserved for flags");
  if (LLVM_LIKELY(!isExternalLength(length))) {
    if (asciiNotUTF16) {
      return DynamicASCIIStringPrimitive::create(runtime, length);
    } else {
      return DynamicUTF16StringPrimitive::create(runtime, length);
    }
  } else {
    if (asciiNotUTF16) {
      return ExternalStringPrimitive<char>::create(runtime, length);
    } else {
      return ExternalStringPrimitive<char16_t>::create(runtime, length);
    }
  }
}

inline CallResult<HermesValue> StringPrimitive::create(
    Runtime &runtime,
    ASCIIRef str) {
  static_assert(
      EXTERNAL_STRING_THRESHOLD < MAX_STRING_LENGTH,
      "External string threshold should be smaller than max string size.");
  if (LLVM_LIKELY(!isExternalLength(str.size()))) {
    return DynamicASCIIStringPrimitive::create(runtime, str);
  } else {
    return ExternalStringPrimitive<char>::create(runtime, arrayToString(str));
  }
}

inline CallResult<HermesValue> StringPrimitive::create(
    Runtime &runtime,
    UTF16Ref str) {
  static_assert(
      EXTERNAL_STRING_THRESHOLD < MAX_STRING_LENGTH,
      "External string threshold should be smaller than max string size.");
  if (LLVM_LIKELY(!isExternalLength(str.size()))) {
    return createDynamic(runtime, str);
  } else {
    return createExternal(runtime, str);
  }
}

inline CallResult<HermesValue> StringPrimitive::createWithKnownEncoding(
    Runtime &runtime,
    UTF16Ref str,
    bool isASCII) {
  static_assert(
      EXTERNAL_STRING_THRESHOLD < MAX_STRING_LENGTH,
      "External string threshold should be smaller than max string size.");
  if (LLVM_LIKELY(!isExternalLength(str.size()))) {
    return createDynamicWithKnownEncoding(runtime, str, isASCII);
  } else {
    return createExternal(runtime, str, isASCII);
  }
}

inline CallResult<HermesValue> StringPrimitive::createLongLived(
    Runtime &runtime,
    ASCIIRef str) {
  static_assert(
      EXTERNAL_STRING_THRESHOLD < MAX_STRING_LENGTH,
      "External string threshold should be smaller than max string size.");
  if (LLVM_LIKELY(!isExternalLength(str.size()))) {
    return DynamicASCIIStringPrimitive::createLongLived(runtime, str);
  } else {
    return ExternalStringPrimitive<char>::createLongLived(
        runtime, arrayToString(str));
  }
}

inline CallResult<HermesValue> StringPrimitive::createLongLived(
    Runtime &runtime,
    UTF16Ref str) {
  static_assert(
      EXTERNAL_STRING_THRESHOLD < MAX_STRING_LENGTH,
      "External string threshold should be smaller than max string size.");
  if (LLVM_LIKELY(!isExternalLength(str.size()))) {
    return DynamicUTF16StringPrimitive::createLongLived(runtime, str);
  } else {
    return ExternalStringPrimitive<char16_t>::createLongLived(
        runtime, arrayToString(str));
  }
}

inline bool StringPrimitive::canBeUniqued() const {
  return vmisa<SymbolStringPrimitive>(this);
}

inline void StringPrimitive::convertToUniqued(hermes::vm::SymbolID uniqueID) {
  assert(canBeUniqued() && "StringPrimitive cannot be uniqued");
  assert(!isUniqued() && "StringPrimitive is already uniqued");
  assert(
      uniqueID.isValid() && uniqueID.isUniqued() &&
      "uniqueID SymbolID is not valid and uniqued");
  this->lengthAndFlags_ |= LENGTH_FLAG_UNIQUED;
  vmcast<SymbolStringPrimitive>(this)->updateUniqueID(uniqueID);
}

template <>
inline const char *StringPrimitive::castToPointer<char>() const {
  return castToASCIIPointer();
}
template <>
inline const char16_t *StringPrimitive::castToPointer<char16_t>() const {
  return castToUTF16Pointer();
}

inline const char *StringPrimitive::castToASCIIPointer() const {
  if (LLVM_UNLIKELY(isExternal())) {
    return vmcast<ExternalASCIIStringPrimitive>(this)->getRawPointer();
  } else if (vmisa<DynamicUniquedASCIIStringPrimitive>(this)) {
    return vmcast<DynamicUniquedASCIIStringPrimitive>(this)->getRawPointer();
  } else if (vmisa<DynamicASCIIStringPrimitive>(this)) {
    return vmcast<DynamicASCIIStringPrimitive>(this)->getRawPointer();
  } else {
    return vmcast<BufferedASCIIStringPrimitive>(this)->getRawPointer();
  }
}

inline const char16_t *StringPrimitive::castToUTF16Pointer() const {
  if (LLVM_UNLIKELY(isExternal())) {
    return vmcast<ExternalUTF16StringPrimitive>(this)->getRawPointer();
  } else if (vmisa<DynamicUniquedUTF16StringPrimitive>(this)) {
    return vmcast<DynamicUniquedUTF16StringPrimitive>(this)->getRawPointer();
  } else if (vmisa<DynamicUTF16StringPrimitive>(this)) {
    return vmcast<DynamicUTF16StringPrimitive>(this)->getRawPointer();
  } else {
    return vmcast<BufferedUTF16StringPrimitive>(this)->getRawPointer();
  }
}

inline char *StringPrimitive::castToASCIIPointerForWrite() {
  if (LLVM_UNLIKELY(isExternal())) {
    return vmcast<ExternalASCIIStringPrimitive>(this)->getRawPointerForWrite();
  } else if (vmisa<DynamicUniquedASCIIStringPrimitive>(this)) {
    return vmcast<DynamicUniquedASCIIStringPrimitive>(this)
        ->getRawPointerForWrite();
  } else {
    return vmcast<DynamicASCIIStringPrimitive>(this)->getRawPointerForWrite();
  }
}

inline char16_t *StringPrimitive::castToUTF16PointerForWrite() {
  if (LLVM_UNLIKELY(isExternal())) {
    return vmcast<ExternalUTF16StringPrimitive>(this)->getRawPointerForWrite();
  } else if (vmisa<DynamicUniquedUTF16StringPrimitive>(this)) {
    return vmcast<DynamicUniquedUTF16StringPrimitive>(this)
        ->getRawPointerForWrite();
  } else {
    return vmcast<DynamicUTF16StringPrimitive>(this)->getRawPointerForWrite();
  }
}

inline SymbolID StringPrimitive::getUniqueID(Runtime &runtime) const {
  assert(this->isUniqued() && "StringPrimitive is not uniqued");
  return vmcast<SymbolStringPrimitive>(this)->getUniqueID(runtime);
}

inline char16_t StringPrimitive::at(uint32_t index) const {
  assert(index < getStringLength() && "Index out of bound");
  if (isASCII()) {
    return *(castToASCIIPointer() + index);
  } else {
    return *(castToUTF16Pointer() + index);
  }
}

inline bool StringPrimitive::isASCII() const {
  // Abstractly, we're doing the following test:
  // return getKind() == CellKind::DynamicASCIIStringPrimitiveKind ||
  //        getKind() == CellKind::DynamicUniquedASCIIStringPrimitiveKind ||
  //        getKind() == CellKind::ExternalASCIIStringPrimitiveKind;
  // We speed this up by making the assumption that the string primitive kinds
  // are defined consecutively, alternating between ASCII and UTF16.
  // We statically enforce this:
  static_assert(
      cellKindsContiguousAscending(
          CellKind::DynamicUTF16StringPrimitiveKind,
          CellKind::DynamicASCIIStringPrimitiveKind,
          CellKind::BufferedUTF16StringPrimitiveKind,
          CellKind::BufferedASCIIStringPrimitiveKind,
          CellKind::DynamicUniquedUTF16StringPrimitiveKind,
          CellKind::DynamicUniquedASCIIStringPrimitiveKind,
          CellKind::ExternalUTF16StringPrimitiveKind,
          CellKind::ExternalASCIIStringPrimitiveKind),
      "Cell kinds in unexpected order");
  // Given this assumption, the ASCII versions are either both odd or both
  // even.
  return (static_cast<uint32_t>(getKind()) & 1u) ==
      (static_cast<uint32_t>(CellKind::DynamicASCIIStringPrimitiveKind) & 1u);
}

inline bool StringPrimitive::isExternal() const {
  // We require that external cell kinds be larger than dynamic cell kinds.
  static_assert(
      cellKindsContiguousAscending(
          CellKind::DynamicUTF16StringPrimitiveKind,
          CellKind::DynamicASCIIStringPrimitiveKind,
          CellKind::BufferedUTF16StringPrimitiveKind,
          CellKind::BufferedASCIIStringPrimitiveKind,
          CellKind::DynamicUniquedUTF16StringPrimitiveKind,
          CellKind::DynamicUniquedASCIIStringPrimitiveKind,
          CellKind::ExternalUTF16StringPrimitiveKind,
          CellKind::ExternalASCIIStringPrimitiveKind),
      "Cell kinds in unexpected order");
  return getKind() >= CellKind::ExternalUTF16StringPrimitiveKind;
}

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_STRINGPRIMITIVE_H

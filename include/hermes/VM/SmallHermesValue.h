/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_SMALLHERMESVALUE_H
#define HERMES_VM_SMALLHERMESVALUE_H

#include "hermes/Support/Algorithms.h"
#include "hermes/Support/OptValue.h"
#include "hermes/VM/CompressedPointer.h"
#include "hermes/VM/GCDecl.h"
#include "hermes/VM/HeapAlign.h"
#include "hermes/VM/HermesValue.h"
#include "hermes/VM/SymbolID.h"
#include "hermes/VM/sh_small_hermes_value.h"

#include "llvh/Support/MathExtras.h"

#include <cassert>
#include <cmath>
#include <cstdint>

namespace hermes {
namespace vm {

class BigIntPrimitive;
class StringPrimitive;
class GCCell;
class Runtime;

/// SmallHermesValue is the HermesValue encoding used on the heap when we want
/// to potentially benefit from a 32-bit representation (depending on native
/// pointer size and HERMESVM_COMPRESSED_POINTERS). A 32-bit representation
/// inherently requires boxing doubles, since they can't fit in 32 bits.
///
/// Additionally, for testing, we can force boxing of doubles even in a 64-bit
/// representation.

#ifndef HERMESVM_BOXED_DOUBLES
/// An adaptor class that provides the API of a SmallHermesValue is internally
/// just a HermesValue.
class SmallHermesValueAdaptor : protected HermesValue {
  constexpr explicit SmallHermesValueAdaptor(HermesValue hv)
      : HermesValue(hv) {}

 public:
  SmallHermesValueAdaptor() = default;
  SmallHermesValueAdaptor(const SmallHermesValueAdaptor &) = default;

#ifdef _MSC_VER
  // This is a workaround to ensure is_trivial is true in MSVC.
  SmallHermesValueAdaptor(SmallHermesValueAdaptor &&) = default;
  SmallHermesValueAdaptor &operator=(const SmallHermesValueAdaptor &hv) =
      default;
#else
  SmallHermesValueAdaptor &operator=(const SmallHermesValueAdaptor &hv) =
      delete;
#endif

  using RawType = HermesValue::RawType;

  using HermesValue::getBool;
  using HermesValue::getRaw;
  using HermesValue::getSymbol;
  using HermesValue::isBigInt;
  using HermesValue::isBool;
  using HermesValue::isEmpty;
  using HermesValue::isNull;
  using HermesValue::isNumber;
  using HermesValue::isObject;
  using HermesValue::isPointer;
  using HermesValue::isString;
  using HermesValue::isSymbol;
  using HermesValue::isUndefined;

  /// \return true if it's a Number since double bits are always inlined in
  /// HermesValue.
  bool isInlinedDouble() const {
    return isNumber();
  }
  /// \return false since HermesValue does not box doubles.
  bool isBoxedDouble() const {
    return false;
  }
  /// This should never be executed since isBoxedDouble() always return false.
  /// Its existence is only to have the same method as HermesValue32 to satisfy
  /// compiler.
  double getBoxedDouble(PointerBase &) const {
    llvm_unreachable("SmallHermesValueAdaptor does not have boxed doubles.");
  }

  template <class T>
  inline T getNumberAs(PointerBase &) const {
    return HermesValue::getNumberAs<T>();
  }

  HermesValue encodeAsHermesValue() const {
    return *this;
  }
  static SmallHermesValueAdaptor decodeFromHermesValue(HermesValue hv) {
    return SmallHermesValueAdaptor{hv};
  }

  HermesValue toHV(PointerBase &) const {
    return *this;
  }
  HermesValue unboxToHV(PointerBase &) const {
    return *this;
  }

  GCCell *getPointer(PointerBase &) const {
    return static_cast<GCCell *>(HermesValue::getPointer());
  }
  CompressedPointer getPointer() const {
    assert(
        sizeof(uintptr_t) == sizeof(CompressedPointer::RawType) &&
        "Adaptor should not be used when compressed pointers are enabled.");
    uintptr_t rawPtr = reinterpret_cast<uintptr_t>(HermesValue::getPointer());
    return CompressedPointer::fromRaw(rawPtr);
  }
  GCCell *getObject(PointerBase &) const {
    return static_cast<GCCell *>(HermesValue::getObject());
  }
  CompressedPointer getObject() const {
    assert(
        sizeof(uintptr_t) == sizeof(CompressedPointer::RawType) &&
        "Adaptor should not be used when compressed pointers are enabled.");
    uintptr_t rawPtr = reinterpret_cast<uintptr_t>(HermesValue::getObject());
    return CompressedPointer::fromRaw(rawPtr);
  }
  StringPrimitive *getString(PointerBase &) const {
    return HermesValue::getString();
  }
  BigIntPrimitive *getBigInt(PointerBase &) const {
    return HermesValue::getBigInt();
  }
  double getNumber(PointerBase &) const {
    return HermesValue::getNumber();
  }
  uint32_t getRelocationID() const {
    return reinterpret_cast<uintptr_t>(HermesValue::getPointer());
  }

  inline void setInGC(SmallHermesValueAdaptor hv, GC &gc);

  SmallHermesValueAdaptor updatePointer(GCCell *ptr, PointerBase &) const {
    return SmallHermesValueAdaptor{HermesValue::updatePointer(ptr)};
  }
  SmallHermesValueAdaptor updatePointer(CompressedPointer ptr) const {
    assert(
        sizeof(uintptr_t) == sizeof(CompressedPointer::RawType) &&
        "Adaptor should not be used when compressed pointers are enabled.");
    GCCell *cellPtr = reinterpret_cast<GCCell *>(ptr.getRaw());
    return SmallHermesValueAdaptor{HermesValue::updatePointer(cellPtr)};
  }
  void unsafeUpdateRelocationID(uint32_t id) {
    HermesValue::unsafeUpdatePointer(
        reinterpret_cast<void *>(static_cast<uintptr_t>(id)));
  }
  void unsafeUpdatePointer(GCCell *ptr, PointerBase &) {
    HermesValue::unsafeUpdatePointer(ptr);
  }

  static constexpr SmallHermesValueAdaptor encodeHermesValue(
      HermesValue hv,
      Runtime &) {
    return SmallHermesValueAdaptor{hv};
  }
  /// Encode \p hv without allocating. \return the encoding, always: this
  /// representation never boxes.
  static OptValue<SmallHermesValueAdaptor> tryEncodeHermesValueNoAlloc(
      HermesValue hv,
      PointerBase &) {
    return SmallHermesValueAdaptor{hv};
  }
  static SmallHermesValueAdaptor encodeBigIntValue(
      BigIntPrimitive *ptr,
      PointerBase *) {
    return SmallHermesValueAdaptor{HermesValue::encodeBigIntValue(ptr)};
  }
  static SmallHermesValueAdaptor encodeNumberValue(double d, Runtime &) {
    return SmallHermesValueAdaptor{HermesValue::encodeTrustedNumberValue(d)};
  }
  static SmallHermesValueAdaptor encodeObjectValue(GCCell *ptr, PointerBase &) {
    return SmallHermesValueAdaptor{HermesValue::encodeObjectValue(ptr)};
  }
  static SmallHermesValueAdaptor encodeObjectValue(CompressedPointer cp) {
    GCCell *cellPtr = reinterpret_cast<GCCell *>(cp.getRaw());
    return SmallHermesValueAdaptor{HermesValue::encodeObjectValue(cellPtr)};
  }
  static SmallHermesValueAdaptor encodeStringValue(
      StringPrimitive *ptr,
      PointerBase &) {
    return SmallHermesValueAdaptor{HermesValue::encodeStringValue(ptr)};
  }
  static SmallHermesValueAdaptor encodeSymbolValue(SymbolID s) {
    return SmallHermesValueAdaptor{HermesValue::encodeSymbolValue(s)};
  }
  static constexpr SmallHermesValueAdaptor encodeBoolValue(bool b) {
    return SmallHermesValueAdaptor{HermesValue::encodeBoolValue(b)};
  }

  static constexpr SmallHermesValueAdaptor encodeNullValue() {
    return SmallHermesValueAdaptor{HermesValue::encodeNullValue()};
  }
  static constexpr SmallHermesValueAdaptor encodeUndefinedValue() {
    return SmallHermesValueAdaptor{HermesValue::encodeUndefinedValue()};
  }
  static constexpr SmallHermesValueAdaptor encodeEmptyValue() {
    return SmallHermesValueAdaptor{HermesValue::encodeEmptyValue()};
  }
  /// Encode the double \p d as an inline HermesValue.
  static SmallHermesValueAdaptor encodeInlineDoubleValueUnsafe(double d) {
    return SmallHermesValueAdaptor{HermesValue::encodeTrustedNumberValue(d)};
  }

  /// Create a SmallHermesValue that has the raw representation 0. This value
  /// must never become visible to user code, and is guaranteed to be ignored by
  /// the GC.
  static constexpr SmallHermesValueAdaptor encodeRawZeroValueUnsafe() {
    return SmallHermesValueAdaptor{HermesValue::fromRaw(0)};
  }

  static bool canInlineDouble(double d) {
    return true;
  }

 protected:
  /// Perform read barrier when the value is Pointer or Symbol, then unbox. This
  /// is only used by WeakSmallHermesValue.
  inline HermesValue unboxToHVWithReadBarrier(PointerBase &pb, GC &gc) const;
};
using SmallHermesValue = SmallHermesValueAdaptor;

#else // #ifndef HERMESVM_BOXED_DOUBLES

/// A compressed HermesValue that is always equal to the size of a
/// CompressedPointer. It uses the least significant bits (guaranteed to be zero
/// by the heap alignment) as a tag to determine what the type of the remaining
/// bits is. Some types may use an additional tag bit to form an "extended tag".
/// Doubles have to be boxed on the heap so that they can then be held as
/// pointers. Native values are not supported.
class HermesValue32 {
 public:
  using RawType = CompressedPointer::RawType;
  using SmiType = std::make_signed<RawType>::type;

  /// Version of the HermesValue32 encoding format.
  /// Changing the format of HermesValue32 requires bumping this version number
  /// and fixing any code that relies on the layout of HermesValue32.
  /// Updated: Mar 11, 2025
  static constexpr size_t kVersion = 1;
  static constexpr size_t kNumRawTypeBits = SH_SHV_RAW_TYPE_BITS;
  static constexpr size_t kNumTagBits = SH_SHV_TAG_BITS;
  static constexpr size_t kNumValueBits = kNumRawTypeBits - kNumTagBits;

  static_assert(kNumTagBits == LogHeapAlign, "Tag bits must match alignment");

  /// A 3 bit tag describing the stored value. Tags that represent multiple
  /// types are distinguished using an additional bit found in the "ETag".
  enum class Tag : uint8_t {
    CompressedHV64 = HV32Tag_CompressedHV64,
    String = HV32Tag_String,
    BigInt = HV32Tag_BigInt,
    Object = HV32Tag_Object,
    BoxedDouble = HV32Tag_BoxedDouble,
    Symbol = HV32Tag_Symbol,
    _Last,

    FirstPointer = String,
    LastPointer = BoxedDouble,
  };

 private:
  RawType raw_;

  static_assert(
      static_cast<uint8_t>(Tag::_Last) <= (1 << kNumTagBits),
      "Cannot have more enum values than tag bits.");

  static constexpr HermesValue32 fromRaw(RawType raw) {
    return HermesValue32(raw);
  }
  /// Helper function to encode a non-pointer value with a given tag.
  static constexpr HermesValue32 fromTagAndValue(Tag tag, RawType value) {
    // The value will have been right-shifted by the tag width, and should
    // have zeroes in the top bits.
    assert(llvh::isUInt<kNumValueBits>(value) && "Value out of range.");
    return fromRaw((value << kNumTagBits) | static_cast<uint8_t>(tag));
  }

  RawType getValue() const {
    return _sh_shv_get_value(raw_);
  }

  double getCompressedDouble() const {
    assert(isInlinedDouble() && "Must be a compressed double.");
    return llvh::BitsToDouble(compressedHV64ToBits());
  }

  Tag getTag() const {
    return static_cast<Tag>(_sh_shv_get_tag(raw_));
  }

  /// Assert that the pointer can be encoded.
  static void validatePointer(RawType rawPtr) {
    assert(llvh::countTrailingZeros(rawPtr) >= 3 && "Pointer low bits are set");
  }

  constexpr explicit HermesValue32(RawType raw) : raw_(raw) {}

  static HermesValue32
  encodePointerImpl(GCCell *ptr, Tag tag, PointerBase &pb) {
    return encodePointerImpl(CompressedPointer::encodeNonNull(ptr, pb), tag);
  }

  static HermesValue32 encodePointerImpl(CompressedPointer ptr, Tag tag) {
    RawType p = ptr.getRaw();
    validatePointer(p);
    return fromRaw(p | static_cast<RawType>(tag));
  }

  /// Encode the pointer \p hv, with the HV32 tag of its HV64 tag. Does not
  /// allocate.
  /// \pre \p hv is a pointer.
  inline static HermesValue32 encodePointerHV64(
      HermesValue hv,
      PointerBase &pb);

  /// Whether the given HV (represented as bits) will be encoded as a
  /// compressed HV64 directly. If it can't be encoded, it will require a heap
  /// allocation to encode as a boxed double.
  /// \pre \p hv is a number or compressible.
  /// \return true if the double can be encoded as a compressed HV64.
  static bool canInlineCompressibleOrNumberHV64(HermesValue hv) {
    assert(hv.isNumberOrCompressible() && "hv must be number or compressible");
#if HERMESVM_SANITIZE_HANDLES != 0
    // If Handle-San is enabled, always box doubles on the heap. This ensures
    // that callers have to treat a HermesValue32 containing a number as a
    // pointer.
    // Non-number HermesValues can be compressed.
    return !hv.isNumber();
#else
    constexpr uint64_t kShiftAmount = 64 - kNumValueBits;
    // If hvRaw is the part that would go into the HV32 value, followed
    // by zeros (i.e., it's equal to a value of kNumValueBits bits
    // right-shifted to the top of the 64 bit value), then we can compress.
    return (llvh::isShiftedUInt<kNumValueBits, kShiftAmount>(hv.getRaw()));
#endif
  }

  uint64_t compressedHV64ToBits() const {
    static_assert((uint64_t)Tag::CompressedHV64 == 0, "Must have zero tag");
    assert(getTag() == Tag::CompressedHV64 && "Must be a compressed HV64");
    return _sh_shv_decompress_hv64(raw_);
  }

  static constexpr HermesValue32 bitsToCompressedHV64(uint64_t bits) {
    assert(
        (llvh::isShiftedUInt<kNumValueBits, 64 - kNumValueBits>(bits)) &&
        "Value out of range.");
    static_assert((uint64_t)Tag::CompressedHV64 == 0, "Must have zero tag");
    // The tag is guaranteed to be 0, so we can just shift to compress.
    return fromRaw(bits >> (64 - kNumRawTypeBits));
  }

  /// Encode a HermesValue that is known to either be compressible or a number
  /// as a HermesValue32. "Compressible" values will be stored inline, and
  /// non-compressible doubles will be allocated on the heap. Always treat this
  /// function as though it may allocate.
  inline static HermesValue32 encodeCompressibleOrNumberHV64(
      HermesValue hv,
      Runtime &runtime);

 public:
  HermesValue32() = default;
  HermesValue32(const HermesValue32 &) = default;

#ifdef _MSC_VER
  // This is a workaround to ensure is_trivial is true in MSVC.
  HermesValue32(HermesValue32 &&) = default;
  HermesValue32 &operator=(const HermesValue32 &hv) = default;
#else
  HermesValue32 &operator=(const HermesValue32 &hv) = delete;
#endif

  bool isPointer() const {
    auto tag = getTag();
    return tag >= Tag::FirstPointer && tag <= Tag::LastPointer;
  }
  bool isObject() const {
    return getTag() == Tag::Object;
  }
  bool isBigInt() const {
    return getTag() == Tag::BigInt;
  }
  bool isString() const {
    return getTag() == Tag::String;
  }
  bool isNumber() const {
    Tag tag = getTag();
    // It's likely to be a CompressedDouble, so check it first.
    return isInlinedDouble() || tag == Tag::BoxedDouble;
  }
  bool isInlinedDouble() const {
    return getTag() == Tag::CompressedHV64 &&
        HermesValue::fromRaw(compressedHV64ToBits()).isNumber();
  }
  bool isBoxedDouble() const {
    return getTag() == Tag::BoxedDouble;
  }
  bool isSymbol() const {
    return getTag() == Tag::Symbol;
  }
  bool isUndefined() const {
    return raw_ == encodeUndefinedValue().raw_;
  }
  bool isEmpty() const {
    return raw_ == encodeEmptyValue().raw_;
  }
  bool isNull() const {
    return raw_ == encodeNullValue().raw_;
  }
  bool isBool() const {
    return raw_ == encodeBoolValue(false).raw_ ||
        raw_ == encodeBoolValue(true).raw_;
  }
  RawType getRaw() const {
    return raw_;
  }

#ifndef HERMESVM_COMPRESSED_POINTERS
  /// Encode this HV32 directly into a HermesValue. If the current tag is
  /// CompressedHV64, the result HermesValue is the decompressed value and has
  /// right tag in it. Otherwise, the result HermesValue has tag RawHV32 and the
  /// data bits are exactly from this HV32.
  HermesValue encodeAsHermesValue() const {
    if (getTag() == Tag::CompressedHV64) {
      return HermesValue::fromRaw(compressedHV64ToBits());
    }
    // On 64 bit platforms, the top 16 bits of rawValue must be zero, so we can
    // directly set the tag RawHV32.
    assert(
        (raw_ & ~HermesValue::kDataMask) == 0 && "raw value exceeds 48 bits");
    return HermesValue::fromTagAndValue(HermesValue::Tag::RawHV32, raw_);
  }
  /// Take the directly encoded HV32 out from \p hv.
  static HermesValue32 decodeFromHermesValue(HermesValue hv) {
    if (hv.isNumberOrCompressible()) {
      return bitsToCompressedHV64(hv.getRaw());
    }
    assert((hv.getTag() == HermesValue::Tag::RawHV32) && "Must be RawHV32");
    return fromRaw(hv.getRaw() & HermesValue::kDataMask);
  }
#else
  /// Encode this HV32 directly into a HermesValue, the low 32 bits are the
  /// raw value of this HV32.
  HermesValue encodeAsHermesValue() const {
    return HermesValue::fromTagAndValue(HermesValue::Tag::RawHV32, raw_);
  }
  /// Take the directly encoded HV32 out from \p hv, which must have tag
  /// RawHV32.
  static HermesValue32 decodeFromHermesValue(HermesValue hv) {
    assert((hv.getTag() == HermesValue::Tag::RawHV32) && "Must be RawHV32");
    static_assert(sizeof(RawType) == sizeof(uint32_t));
    return fromRaw((uint32_t)hv.getRaw());
  }
#endif

  /// Convert this to a full HermesValue, but do not unbox a BoxedDouble.
  /// This is only intended for diagnostics or for code reuse in the GC.
  inline HermesValue toHV(PointerBase &pb) const;

  /// Convert this to a full HermesValue, and unbox it if it is currently boxed.
  /// This is more commonly useful, and is essentially the reverse process of
  /// encodeHermesValue.
  inline HermesValue unboxToHV(PointerBase &pb) const;

  /// Methods to access pointer values.
  GCCell *getPointer(PointerBase &pb) const {
    assert(isPointer());
    return getPointer().getNonNull(pb);
  }
  GCCell *getObject(PointerBase &pb) const {
    assert(isObject());
    return getPointer(pb);
  }
  CompressedPointer getObject() const {
    assert(isObject());
    return getPointer();
  }

  inline BigIntPrimitive *getBigInt(PointerBase &pb) const;
  inline StringPrimitive *getString(PointerBase &pb) const;
  inline double getNumber(PointerBase &pb) const;
  inline double getBoxedDouble(PointerBase &pb) const;

  template <class T>
  inline typename std::enable_if<std::is_integral<T>::value, T>::type
  getNumberAs(PointerBase &pb) const {
    double num = getNumber(pb);
    assert(
        num >= std::numeric_limits<T>::min() &&
        // The cast is to ignore the following warning:
        // implicit conversion from 'int64_t' to 'double' changes value.
        num <= (double)std::numeric_limits<T>::max() && (T)num == num &&
        "value not representable as type");
    return num;
  }

  template <class T>
  inline typename std::enable_if<!std::is_integral<T>::value, T>::type
  getNumberAs(PointerBase &pb) const {
    return getNumber(pb);
  }

  CompressedPointer getPointer() const {
    assert(isPointer());
    return getPointerBitsUnsafe();
  }

  /// \return the raw value with the tag bits cleared. If this value is a
  /// pointer, this is the same as getPointer(). Otherwise, the result is not a
  /// valid pointer and must never be dereferenced, but it may be compared, for
  /// instance against the start of a heap segment.
  CompressedPointer getPointerBitsUnsafe() const {
    return CompressedPointer::fromRaw(_sh_shv_get_pointer(raw_).raw);
  }

  SymbolID getSymbol() const {
    assert(isSymbol());
    return SymbolID::unsafeCreate(getValue());
  }
  bool getBool() const {
    assert(isBool());
    return HermesValue::fromRaw(compressedHV64ToBits()).getBool();
  }

  inline void setInGC(HermesValue32 hv, GC &gc);

  HermesValue32 updatePointer(GCCell *ptr, PointerBase &pb) const {
    return encodePointerImpl(ptr, getTag(), pb);
  }
  void unsafeUpdatePointer(GCCell *ptr, PointerBase &pb) {
    setNoBarrier(encodePointerImpl(ptr, getTag(), pb));
  }
  HermesValue32 updatePointer(CompressedPointer ptr) const {
    assert(isPointer());
    return encodePointerImpl(ptr, getTag());
  }

  /// Convert a normal HermesValue to a HermesValue32. If \p hv is a pointer,
  /// this function is guaranteed to not allocate. This is important since an
  /// allocation may invalidate the pointer. For non-pointer values, always
  /// treat this function as though it may allocate.
  inline static HermesValue32 encodeHermesValue(
      HermesValue hv,
      Runtime &runtime);

  /// Convert \p hv to a HermesValue32 without allocating. \return the
  /// encoding, the same as encodeHermesValue's, or none exactly when
  /// encodeHermesValue would box a double.
  inline static OptValue<HermesValue32> tryEncodeHermesValueNoAlloc(
      HermesValue hv,
      PointerBase &pb);

  /// Encode a double as a HermesValue32. Small integer values will be stored
  /// inline and doubles will be allocated on the heap. Always treat this
  /// function as though it may allocate.
  inline static HermesValue32 encodeNumberValue(double d, Runtime &runtime);

  /// Encode the double \p d as an inline HermesValue32.
  /// Convenient because it doesn't require a Runtime reference.
  /// \pre \p d can be inlined in a HermesValue32.
  static HermesValue32 encodeInlineDoubleValueUnsafe(double d) {
    assert(canInlineDouble(d) && "Value out of range.");
    HermesValue hv = HermesValue::encodeTrustedNumberValue(d);
    return bitsToCompressedHV64(hv.getRaw());
  }

  inline static HermesValue32 encodeObjectValue(GCCell *ptr, PointerBase &pb);
  static HermesValue32 encodeObjectValue(CompressedPointer cp) {
    return encodePointerImpl(cp, Tag::Object);
  }

  static HermesValue32 encodeBigIntValue(
      BigIntPrimitive *ptr,
      PointerBase &pb) {
    return encodePointerImpl(reinterpret_cast<GCCell *>(ptr), Tag::BigInt, pb);
  }

  static HermesValue32 encodeStringValue(
      StringPrimitive *ptr,
      PointerBase &pb) {
    return encodePointerImpl(reinterpret_cast<GCCell *>(ptr), Tag::String, pb);
  }

  static HermesValue32 encodeSymbolValue(SymbolID s) {
    return fromTagAndValue(Tag::Symbol, s.unsafeGetRaw());
  }
  static constexpr HermesValue32 encodeBoolValue(bool b) {
    return bitsToCompressedHV64(HermesValue::encodeBoolValue(b).getRaw());
  }
  static constexpr HermesValue32 encodeNullValue() {
    return bitsToCompressedHV64(HermesValue::encodeNullValue().getRaw());
  }
  static constexpr HermesValue32 encodeUndefinedValue() {
    return bitsToCompressedHV64(HermesValue::encodeUndefinedValue().getRaw());
  }
  static constexpr HermesValue32 encodeEmptyValue() {
    return bitsToCompressedHV64(HermesValue::encodeEmptyValue().getRaw());
  }

  /// Create a SmallHermesValue that has the raw representation 0. This value
  /// must never become visible to user code, and is guaranteed to be ignored by
  /// the GC.
  static constexpr HermesValue32 encodeRawZeroValueUnsafe() {
    return HermesValue32{0};
  }

  /// Whether the given double can be encoded as a compressed HV64 directly.
  /// If it can't be encoded, it will require a heap allocation to encode as a
  /// boxed double.
  /// \return true if the double can be encoded as a compressed HV64.
  static bool canInlineDouble(double d) {
    return canInlineCompressibleOrNumberHV64(
        HermesValue::encodeTrustedNumberValue(d));
  }

 protected:
  /// Performs an assignment without a barrier, in cases where the RHS
  /// value may contain an object pointer.  WARNING: this is very
  /// dangerous, and should be avoided whenever possible.  Exposed here
  /// for use by subtypes.
  inline void setNoBarrier(HermesValue32 other) {
    raw_ = other.raw_;
  }

  /// Convert this to a full HermesValue, and unbox it if it is currently boxed.
  /// The conversion is the same as unboxToHV, but it also performs a read
  /// barrier if the value is pointer or symbol. This is only used by
  /// WeakSmallHermesValue, where we need to hold the read pointer/symbol alive.
  inline HermesValue unboxToHVWithReadBarrier(PointerBase &pb, GC &gc) const;
};
using SmallHermesValue = HermesValue32;

#endif // #ifndef HERMESVM_BOXED_DOUBLES

static_assert(
    std::is_trivial<SmallHermesValue>::value,
    "SmallHermesValue must be trivial");

/// Base type for GC aware SmallHermesValues. This should only be used when we
/// don't need to handle large allocation specially.
using GCSmallHermesValueBase = GCHermesValueBaseImpl<SmallHermesValue>;

/// GCSmallHermesValue stored in a normal object.
using GCSmallHermesValue = GCHermesValueImpl<SmallHermesValue>;

/// GCSmallHermesValue stored in an object that supports large allocation.
using GCSmallHermesValueInLargeObj =
    GCHermesValueInLargeObjImpl<SmallHermesValue>;

/// A SmallHermesValue which is stored in non-moveable memory and is known to
/// the garbage collector.
class PinnedSmallHermesValue : public SmallHermesValue {
 public:
  constexpr PinnedSmallHermesValue()
      : PinnedSmallHermesValue(encodeUndefinedValue()) {}
  explicit constexpr PinnedSmallHermesValue(SmallHermesValue v)
      : SmallHermesValue(v) {}
  constexpr PinnedSmallHermesValue(const PinnedSmallHermesValue &) = default;
  PinnedSmallHermesValue &operator=(const PinnedSmallHermesValue &phv) {
    setNoBarrier(phv);
    return *this;
  }
  PinnedSmallHermesValue &operator=(const SmallHermesValue &shv) {
    setNoBarrier(shv);
    return *this;
  }
} HERMES_ATTRIBUTE_WARN_UNUSED_VARIABLES;

} // end namespace vm
} // end namespace hermes

#endif // HERMES_VM_HERMESVALUE_H

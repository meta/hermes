/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VM_SMALLHERMESVALUE_INLINE_H
#define HERMES_VM_SMALLHERMESVALUE_INLINE_H

#include "hermes/VM/SmallHermesValue.h"

#include "hermes/Support/SlowAssert.h"
#include "hermes/VM/BigIntPrimitive.h"
#include "hermes/VM/BoxedDouble.h"
#include "hermes/VM/HermesValue-inline.h"
#include "hermes/VM/StringPrimitive.h"

namespace hermes {
namespace vm {

#ifndef HERMESVM_BOXED_DOUBLES

void SmallHermesValueAdaptor::setInGC(SmallHermesValueAdaptor hv, GC &gc) {
  HermesValue::setInGC(hv, gc);
}

HermesValue SmallHermesValueAdaptor::unboxToHVWithReadBarrier(
    PointerBase &pb,
    GC &gc) const {
  if (isPointer()) {
    gc.weakRefReadBarrier(getPointer(pb));
  } else if (isSymbol()) {
    gc.weakRefReadBarrier(getSymbol());
  }
  return *this;
}

#else // #ifndef HERMESVM_BOXED_DOUBLES

void HermesValue32::setInGC(HermesValue32 hv, GC &gc) {
  setNoBarrier(hv);
  assert(gc.calledByGC());
}

HermesValue HermesValue32::unboxToHVWithReadBarrier(PointerBase &pb, GC &gc)
    const {
  // Since we need to perform read barrier with gc, we can't use the same
  // unified inline path `_sh_shv_unbox_inline` as used by unboxToHV.

  // unboxing depends on the bits layout of HermesValue tags.
  static_assert(HERMESVALUE_VERSION == 2, "HermesValue version mismatch");

  auto tag = getTag();
  // We check for CompressedHV64 and pointers first, because we know that they
  // comprise the overwhelming majority of values.
  if (tag == Tag::CompressedHV64)
    return HermesValue::fromRaw(compressedHV64ToBits());
  if (tag <= Tag::Object) {
    // This must be a pointer tag, since the only tag before the first pointer
    // tag is CompressedHV64, which we have already checked for.
    assert(tag == Tag::Object || tag == Tag::BigInt || tag == Tag::String);
    auto toHV64Tag = [](Tag tag) {
      // Compute the HV64 tag by applying an offset to the HV32 tag.
      auto offs = (uint32_t)HermesValue::Tag::Str - (uint32_t)Tag::String;
      // Note that we can use | here instead of + here because we know that the
      // HV32 pointer tags are only 2 bits, and the two low bits of offs are 0.
      // At least on arm64, this meaningfully improves the generated code.
      return (HermesValue::Tag)(offs | (uint32_t)tag);
    };
    // Check that the calculation is correct.
    static_assert(toHV64Tag(Tag::Object) == HermesValue::Tag::Object);
    static_assert(toHV64Tag(Tag::BigInt) == HermesValue::Tag::BigInt);
    static_assert(toHV64Tag(Tag::String) == HermesValue::Tag::Str);
    auto *ptr = getPointer(pb);
    // Perform a read barrier on the pointer.
    gc.weakRefReadBarrier(ptr);
    return HermesValue::fromTagAndValue(toHV64Tag(tag), (uint64_t)ptr);
  }
  if (tag == Tag::Symbol) {
    auto symbol = getSymbol();
    // Perform a read barrier on the symbol.
    gc.weakRefReadBarrier(symbol);
    return HermesValue::encodeSymbolValue(symbol);
  }

  assert(tag == Tag::BoxedDouble);
  // Read barrier is not needed for boxed double since we are unboxing it now.
  return HermesValue::encodeTrustedNumberValue(
      vmcast<BoxedDouble>(getPointer(pb))->get());
}

HermesValue HermesValue32::unboxToHV(PointerBase &pb) const {
  assert(_sh_shv_tag_to_hv64_tag(HV32Tag_Object) == HVTag_Object);
  assert(_sh_shv_tag_to_hv64_tag(HV32Tag_BigInt) == HVTag_BigInt);
  assert(_sh_shv_tag_to_hv64_tag(HV32Tag_String) == HVTag_Str);

  SHRuntime *shr = &pb;
  SHLegacyValue result = _sh_shv_unbox_inline(shr, raw_);
  return *toPHV(&result);
}

HermesValue HermesValue32::toHV(PointerBase &pb) const {
  if (getTag() == Tag::BoxedDouble)
    return HermesValue::encodeObjectValue(getPointer(pb));
  return unboxToHV(pb);
}

BigIntPrimitive *HermesValue32::getBigInt(PointerBase &pb) const {
  assert(isBigInt());
  return vmcast<BigIntPrimitive>(getPointer(pb));
}

StringPrimitive *HermesValue32::getString(PointerBase &pb) const {
  assert(isString());
  return vmcast<StringPrimitive>(getPointer(pb));
}

double HermesValue32::getNumber(PointerBase &pb) const {
  assert(isNumber());
  if (LLVM_LIKELY(getTag() == Tag::CompressedHV64))
    return getCompressedDouble();
  return vmcast<BoxedDouble>(getPointer(pb))->get();
}

double HermesValue32::getBoxedDouble(PointerBase &pb) const {
  assert(isBoxedDouble());
  return vmcast<BoxedDouble>(getPointer(pb))->get();
}

/* static */ HermesValue32 HermesValue32::encodeHermesValue(
    HermesValue hv,
    Runtime &runtime) {
#if HERMESVM_SANITIZE_HANDLES != 0
  // When Handle-SAN is on, make a double so that this function always
  // allocates. Note we can't do this for pointer values since they would get
  // invalidated.
  if (!hv.isPointer())
    encodeNumberValue(0.0, runtime);
#endif

  // We check for CompressedHV64 and pointers first, because we know that they
  // comprise the overwhelming majority of values.
  if (hv.isNumberOrCompressible())
    return encodeCompressibleOrNumberHV64(hv, runtime);

  if (hv.isPointer())
    return encodePointerHV64(hv, runtime);
  assert(hv.isSymbol() && "Must be a symbol");
  return encodeSymbolValue(hv.getSymbol());
}

/* static */ OptValue<HermesValue32> HermesValue32::tryEncodeHermesValueNoAlloc(
    HermesValue hv,
    PointerBase &pb) {
  // Follow encodeHermesValue, but return none where it would box.
  if (hv.isNumberOrCompressible()) {
    if (LLVM_LIKELY(canInlineCompressibleOrNumberHV64(hv)))
      return bitsToCompressedHV64(hv.getRaw());
    return llvh::None;
  }

  if (hv.isPointer())
    return encodePointerHV64(hv, pb);
  assert(hv.isSymbol() && "Must be a symbol");
  return encodeSymbolValue(hv.getSymbol());
}

/* static */ HermesValue32 HermesValue32::encodePointerHV64(
    HermesValue hv,
    PointerBase &pb) {
  auto tag = hv.getTag();
  // Make sure we have accounted for all the HV tags.
  assert(
      tag == HermesValue::Tag::Str || tag == HermesValue::Tag::BigInt ||
      tag == HermesValue::Tag::Object);
  auto toHV32Tag = [](HermesValue::Tag tag) {
    // Compute the HV32 tag by applying an offset to the HV64 tag.
    auto offs = (uint32_t)HermesValue::Tag::Str - (uint32_t)Tag::String;
    return (Tag)((uint32_t)tag - offs);
  };
  // Check that the calculation is correct.
  static_assert(toHV32Tag(HermesValue::Tag::Object) == Tag::Object);
  static_assert(toHV32Tag(HermesValue::Tag::BigInt) == Tag::BigInt);
  static_assert(toHV32Tag(HermesValue::Tag::Str) == Tag::String);
  return encodePointerImpl((GCCell *)hv.getPointer(), toHV32Tag(tag), pb);
}

/* static */ HermesValue32 HermesValue32::encodeCompressibleOrNumberHV64(
    HermesValue hv,
    Runtime &runtime) {
  const uint64_t hvRaw = hv.getRaw();

  if (LLVM_LIKELY(canInlineCompressibleOrNumberHV64(hv)))
    return bitsToCompressedHV64(hvRaw);

  assert(hv.isNumber() && "Must be compressible or number HV64");

  return encodePointerImpl(
      BoxedDouble::create(hv.getNumber(), runtime), Tag::BoxedDouble, runtime);
}

/* static */ HermesValue32 HermesValue32::encodeNumberValue(
    double d,
    Runtime &runtime) {
  return encodeCompressibleOrNumberHV64(
      HermesValue::encodeTrustedNumberValue(d), runtime);
}

/* static */ HermesValue32 HermesValue32::encodeObjectValue(
    GCCell *ptr,
    PointerBase &pb) {
  assert(
      (!ptr || !vmisa<StringPrimitive>(ptr) || !vmisa<BigIntPrimitive>(ptr)) &&
      "Strings must use encodeStringValue; BigInts, encodeBigIntValue");
  return encodePointerImpl(ptr, Tag::Object, pb);
}

#endif // #ifndef HERMESVM_BOXED_DOUBLES

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_SMALLHERMESVALUE_INLINE_H

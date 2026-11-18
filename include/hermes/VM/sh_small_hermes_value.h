/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "hermes/VM/sh_legacy_value.h"
#include "hermes/VM/sh_mirror.h"
#include "hermes/VM/sh_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HERMESVM_BOXED_DOUBLES

/// C enum mirroring HermesValue32::Tag from SmallHermesValue.h.
enum HV32Tag {
  HV32Tag_CompressedHV64 = 0,
  HV32Tag_String = 1,
  HV32Tag_BigInt = 2,
  HV32Tag_Object = 3,
  HV32Tag_BoxedDouble = 4,
  HV32Tag_Symbol = 5,
};

/// Number of bits used for the tag (mirrors kNumTagBits from
/// SmallHermesValue.h)
#define SH_SHV_TAG_BITS 3

/// Number of bits used by the raw type.
#define SH_SHV_RAW_TYPE_BITS (sizeof(SHCompressedPointerRawType) * 8)

/// Mask to extract the tag bits.
#define SH_SHV_TAG_MASK ((1 << SH_SHV_TAG_BITS) - 1)

/// \return the tag from a SmallHermesValue (low SH_SHV_TAG_BITS bits).
static inline enum HV32Tag _sh_shv_get_tag(SHGCSmallHermesValue shv) {
  return (enum HV32Tag)(shv & SH_SHV_TAG_MASK);
}

/// \return the value portion from a SmallHermesValue (upper bits).
static inline uint32_t _sh_shv_get_value(SHGCSmallHermesValue shv) {
  return shv >> SH_SHV_TAG_BITS;
}

/// \return a compressed pointer from a SmallHermesValue (masking out tag bits).
static inline SHCompressedPointer _sh_shv_get_pointer(
    SHGCSmallHermesValue shv) {
  SHCompressedPointer cp = {.raw = shv & ~(SH_SHV_TAG_MASK)};
  return cp;
}

/// \return a full 64-bit value given a CompressedHV64 SmallHermesValue.
static inline uint64_t _sh_shv_decompress_hv64(SHGCSmallHermesValue shv) {
  return ((uint64_t)shv) << (64 - SH_SHV_RAW_TYPE_BITS);
}

/// \return the HV64 tag for the given \p shvTag.
static inline enum HVTag _sh_shv_tag_to_hv64_tag(enum HV32Tag shvTag) {
  // Compute the HV64 tag by applying an offset to the HV32 tag.
  // Note that we can use | here instead of + here because we know that the
  // HV32 pointer tags are only 2 bits, and the two low bits of offs are 0.
  // At least on arm64, this meaningfully improves the generated code.
  uint32_t offs = (uint32_t)HVTag_Str - (uint32_t)HV32Tag_String;
  return (enum HVTag)(offs | (uint32_t)shvTag);
}

/// Convert SHGCSmallHermesValue to SHLegacyValue (mirrors
/// HermesValue32::unboxToHV)
static inline SHLegacyValue _sh_shv_unbox_inline(
    SHRuntime *shr,
    SHGCSmallHermesValue shv) {
  enum HV32Tag tag = _sh_shv_get_tag(shv);

  if (tag == HV32Tag_CompressedHV64) {
    SHLegacyValue result = {.raw = _sh_shv_decompress_hv64(shv)};
    return result;
  }

  if (tag <= HV32Tag_Object) {
    // This must be a pointer tag, since the only tag before the first pointer
    // tag is CompressedHV64, which we have already checked for.
    SHCompressedPointer cp = _sh_shv_get_pointer(shv);
    void *ptr = _sh_cp_decode(shr, cp);
    enum HVTag tagHV64 = _sh_shv_tag_to_hv64_tag(tag);
    return _sh_ljs_encode_raw_tag((uint64_t)(uintptr_t)ptr, tagHV64);
  }

  if (tag == HV32Tag_Symbol) {
    uint32_t symbolValue = _sh_shv_get_value(shv);
    return _sh_ljs_encode_raw_etag((uint64_t)symbolValue, HVETag_Symbol);
  }

  SHCompressedPointer cp = _sh_shv_get_pointer(shv);
  SHBoxedDouble *boxedDouble = (SHBoxedDouble *)_sh_cp_decode(shr, cp);
  return _sh_ljs_double(boxedDouble->value_);
}

/// Like _sh_shv_unbox_inline, but \p shv must be a pointer.
static inline SHLegacyValue _sh_shv_unbox_ptr_inline(
    SHRuntime *shr,
    SHGCSmallHermesValue shv) {
  enum HV32Tag tag = _sh_shv_get_tag(shv);
  assert(
      tag != HV32Tag_CompressedHV64 && tag <= HV32Tag_Object &&
      "value must be a pointer");
  void *ptr = _sh_cp_decode_non_null(shr, _sh_shv_get_pointer(shv));
  return _sh_ljs_encode_raw_tag(
      (uint64_t)(uintptr_t)ptr, _sh_shv_tag_to_hv64_tag(tag));
}

/// Like _sh_shv_unbox_inline, but \p shv must not be a number or symbol.
static inline SHLegacyValue _sh_shv_unbox_nonnumsym_inline(
    SHRuntime *shr,
    SHGCSmallHermesValue shv) {
  if (_sh_shv_get_tag(shv) == HV32Tag_CompressedHV64) {
    SHLegacyValue result = {.raw = _sh_shv_decompress_hv64(shv)};
    return result;
  }
  return _sh_shv_unbox_ptr_inline(shr, shv);
}

#else

/// Convert SHGCSmallHermesValue to SHLegacyValue (mirrors
/// HermesValue32::unboxToHV)
static inline SHLegacyValue _sh_shv_unbox_inline(
    SHRuntime *shr,
    SHGCSmallHermesValue shv) {
  // When HERMESVM_BOXED_DOUBLES is disabled, SHGCSmallHermesValue is
  // SHLegacyValue So this is a no-op, just return the argument
  (void)shr; // unused parameter
  return shv;
}

/// Like _sh_shv_unbox_inline, but \p shv must be a pointer.
static inline SHLegacyValue _sh_shv_unbox_ptr_inline(
    SHRuntime *shr,
    SHGCSmallHermesValue shv) {
  (void)shr;
  return shv;
}

/// Like _sh_shv_unbox_inline, but \p shv must not be a number or symbol.
static inline SHLegacyValue _sh_shv_unbox_nonnumsym_inline(
    SHRuntime *shr,
    SHGCSmallHermesValue shv) {
  (void)shr;
  return shv;
}

#endif // HERMESVM_BOXED_DOUBLES

#ifdef __cplusplus
}
#endif

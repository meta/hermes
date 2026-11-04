/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "hermes/VM/JIT/Config.h"
#if HERMESVM_JIT

#include "JitEmitter.h"

#include "../RuntimeOffsets.h"

namespace hermes::vm::arm64 {

// Ensure that HermesValue tags are handled correctly by updating this every
// time the HERMESVALUE_VERSION changes, and going through the JIT and updating
// any relevant code.
static_assert(
    HERMESVALUE_VERSION == 2,
    "HermesValue version mismatch, JIT may need to be updated");

/// Get the tag for the HermesValue in \p xIn and place it in \p xOut.
/// xOut and xIn may be the same, but if they are, xIn will be clobbered.
inline void emit_sh_ljs_get_tag(
    a64::Assembler &a,
    const a64::GpX &xOut,
    const a64::GpX &xIn) {
  static_assert(
      HERMESVALUE_VERSION == 2,
      "kHV_NumDataBits is 48 and can be easily shifted");
  a.asr(xOut, xIn, kHV_NumDataBits);
}

/// Check whether \p xTagReg is the tag for a pointer value.
/// CPU flags are updated as result. b.hs success.
inline void emit_sh_ljs_tag_is_pointer(
    a64::Assembler &a,
    const a64::GpX &xTagReg) {
  static_assert(
      HERMESVALUE_VERSION == 2,
      "All tags above HVTag_FirstPointer are pointers");
  // The valid pointer tags are: 0xfd (HVTag_FirstPointer) to 0xff
  // (HVTag_Last), but all sign extended to 64 bits.
  // We need an unsigned comparison (tag >= 0xfd) to catch all pointers.
  // Doubles may have 0 in the tag bits, e.g. so we have to use
  // unsigned condition codes to make sure they don't get detected as pointers.
  a.cmn(xTagReg, -HVTag_FirstPointer);
}

/// Emit code to check whether the input reg is an object tag,
/// using the specified register.
/// The input reg is not modified.
/// CPU flags are updated as result. b.eq on success.
inline void emit_sh_ljs_tag_is_object(
    a64::Assembler &a,
    const a64::GpX &xTagReg) {
  static_assert(
      (int16_t)HVTag_Object == (int16_t)(-1), "HVTag_Object must be -1");
  a.cmn(xTagReg, -HVTag_Object);
}

/// Emit code to check whether the input reg is string tag, using the specified
/// register.
/// The input reg is not modified.
/// CPU flags are updated as result. b.eq on success.
inline void emit_sh_ljs_tag_is_string(
    a64::Assembler &a,
    const a64::GpX &xTagReg) {
  static_assert((int16_t)HVTag_Str == (int16_t)(-3), "HVTag_Str must be -3");
  a.cmn(xTagReg, -HVTag_Str);
}

inline void emit_sh_ljs_get_pointer(
    a64::Assembler &a,
    const a64::GpX &xOut,
    const a64::GpX &xIn) {
  // See:
  // https://dinfuehr.github.io/blog/encoding-of-immediate-values-on-aarch64/
  static_assert(
      HERMESVALUE_VERSION == 2,
      "kHV_DataMask is 0x000...1111... and can be encoded as a logical immediate");
  a.and_(xOut, xIn, kHV_DataMask);
}

/// Encode a native pointer into a tagged object pointer (SHLegacyValue).
/// The same register is used for input and output.
inline void emit_sh_ljs_object(a64::Assembler &a, const a64::GpX &inOut) {
  static_assert(
      HERMESVALUE_VERSION == 2,
      "HVTag_Object << kHV_NumDataBits is 0x1111...0000... and can be encoded as a logical immediate");
  a.movk(inOut, (uint16_t)HVTag_Object, kHV_NumDataBits);
}

/// Encode a native pointer into a tagged object pointer (SHLegacyValue).
/// Takes an input and output register, which can be the same.
/// In some sense this supersedes
inline void emit_sh_ljs_object2(
    a64::Assembler &a,
    const a64::GpX &xOut,
    const a64::GpX &xIn) {
  static_assert(
      HERMESVALUE_VERSION == 2,
      "HVTag_Object << kHV_NumDataBits is 0x1111...0000... and can be encoded as a logical immediate");
  a.orr(xOut, xIn, (uint64_t)HVTag_Object << kHV_NumDataBits);
}

/// Emit code to check whether the input HermesValue is a double.
/// The input reg is not modified.
/// The temp reg is modified.
/// CPU flags are updated as result. b.lo on success.
inline void emit_sh_ljs_is_double(
    a64::Assembler &a,
    const a64::GpX &xInput,
    const a64::GpX &xTmp) {
  static_assert(
      HERMESVALUE_VERSION == 2,
      "numbers must be lower than HVTag_First << kHV_NumDataBits");
  a.mov(xTmp, ((uint64_t)HVTag_First << kHV_NumDataBits));
  a.cmp(xInput, xTmp);
}

/// Encode a string pointer into a tagged string (SHLegacyValue).
/// The same register is used for input and output.
inline void emit_sh_ljs_string(a64::Assembler &a, const a64::GpX &inOut) {
  static_assert(
      HERMESVALUE_VERSION == 2,
      "HVTag_Str << kHV_NumDataBits is 0x1101...0000... and can be encoded as a logical immediate");
  a.movk(inOut, (uint16_t)HVTag_Str, kHV_NumDataBits);
}

/// Encode a string pointer into a tagged string (SmallHermesValue).
/// The same register is used for input and output.
inline void emit_shv_string(a64::Assembler &a, const a64::GpX &inOut) {
#ifdef HERMESVM_BOXED_DOUBLES
  static_assert(
      SmallHermesValue::kVersion == 1, "String tagging requires simple or");
  a.orr(inOut, inOut, HermesValue32::Tag::String);
#else
  emit_sh_ljs_string(a, inOut);
#endif
}

/// Emit code to check whether the input reg is an object, using the specified
/// temp register. The input reg is not
/// modified unless it is the same as the temp, which is allowed.
/// CPU flags are updated as result. b.eq on success.
inline void emit_sh_ljs_is_object(
    a64::Assembler &a,
    const a64::GpX &xTempReg,
    const a64::GpX &xInputReg) {
  emit_sh_ljs_get_tag(a, xTempReg, xInputReg);
  emit_sh_ljs_tag_is_object(a, xTempReg);
}

/// Emit code to check whether the input HermesValue is a string,
/// using the specified temp register. The input reg is not
/// modified unless it is the same as the temp, which is allowed.
/// CPU flags are updated as result. b.eq on success.
inline void emit_sh_ljs_is_string(
    a64::Assembler &a,
    const a64::GpX &xTempReg,
    const a64::GpX &xInputReg) {
  emit_sh_ljs_get_tag(a, xTempReg, xInputReg);
  emit_sh_ljs_tag_is_string(a, xTempReg);
}

/// Emit code to check whether the input reg is a bigint, using the specified
/// temp register. The input reg is not
/// modified unless it is the same as the temp, which is allowed.
/// CPU flags are updated as result. b.eq on success.
inline void emit_sh_ljs_is_bigint(
    a64::Assembler &a,
    const a64::GpX &xTempReg,
    const a64::GpX &xInputReg) {
  // Get the tag bits by right shifting.
  static_assert(
      (int16_t)HVTag_BigInt == (int16_t)(-2), "HVTag_BigInt must be -2");
  a.asr(xTempReg, xInputReg, kHV_NumDataBits);
  a.cmn(xTempReg, -HVTag_BigInt);
}

/// Emit code to check whether the input reg is empty, using the specified
/// temp register.
/// The input reg is not modified unless it is the same as the temp,
/// which is allowed.
/// CPU flags are updated as result. b.eq on success.
inline void emit_sh_ljs_is_empty(
    a64::Assembler &a,
    const a64::GpX &xTempReg,
    const a64::GpX &xInputReg) {
  // Get the tag bits by right shifting.
  static_assert(
      (int16_t)HVETag_Empty == (int16_t)(-14), "HVETag_Empty must be -14");
  a.asr(xTempReg, xInputReg, kHV_NumDataBits - 1);
  a.cmn(xTempReg, -HVETag_Empty);
}

/// Emit code to check whether the input reg is null, using the specified
/// temp register.
/// The input reg is not modified unless it is the same as the temp,
/// which is allowed.
/// CPU flags are updated as result. b.eq on success.
inline void emit_sh_ljs_is_null(
    a64::Assembler &a,
    const a64::GpX &xTempReg,
    const a64::GpX &xInputReg) {
  // Get the tag bits by right shifting.
  static_assert(
      (int16_t)HVETag_Null == (int16_t)(-11), "HVETag_Null must be -11");
  a.asr(xTempReg, xInputReg, kHV_NumDataBits - 1);
  a.cmn(xTempReg, -HVETag_Null);
}

/// Emit code to check whether the input reg is bool, using the specified
/// temp register.
/// The input reg is not modified unless it is the same as the temp,
/// which is allowed.
/// CPU flags are updated as result. b.eq on success.
inline void emit_sh_ljs_is_bool(
    a64::Assembler &a,
    const a64::GpX &xTempReg,
    const a64::GpX &xInputReg) {
  // Get the tag bits by right shifting.
  static_assert(
      (int16_t)HVETag_Bool == (int16_t)(-10), "HVETag_Bool must be -10");
  a.asr(xTempReg, xInputReg, kHV_NumDataBits - 1);
  a.cmn(xTempReg, -HVETag_Bool);
}

/// Emit code to check whether the input reg is undefined, using the specified
/// temp register.
/// The input reg is not modified unless it is the same as the temp,
/// which is allowed.
/// CPU flags are updated as result. b.eq on success.
inline void emit_sh_ljs_is_undefined(
    a64::Assembler &a,
    const a64::GpX &xTempReg,
    const a64::GpX &xInputReg) {
  // Get the tag bits by right shifting.
  static_assert(
      HERMESVALUE_VERSION == 2,
      "HVETag_Undefined must be at kHV_NumDataBits - 1");
  static_assert(
      (int16_t)HVETag_Undefined == (int16_t)(-12),
      "HVETag_Undefined must be -12");
  a.asr(xTempReg, xInputReg, kHV_NumDataBits - 1);
  a.cmn(xTempReg, -HVETag_Undefined);
}

/// Emit code to check whether the input reg is Symbol, using the specified
/// temp register.
/// The input reg is not modified unless it is the same as the temp,
/// which is allowed.
/// CPU flags are updated as result. b.eq on success.
inline void emit_sh_ljs_is_symbol(
    a64::Assembler &a,
    const a64::GpX &xTempReg,
    const a64::GpX &xInputReg) {
  // Get the tag bits by right shifting.
  static_assert(
      HERMESVALUE_VERSION == 2, "HVETag_Symbol must be at kHV_NumDataBits - 1");
  static_assert(
      (int16_t)HVETag_Symbol == (int16_t)(-9), "HVETag_Symbol must be -9");
  a.asr(xTempReg, xInputReg, kHV_NumDataBits - 1);
  a.cmn(xTempReg, -HVETag_Symbol);
}

/// For a register \p inOut that contains a bool (i.e. either 0 or 1), turn it
/// into a HermesValue boolean by adding the corresponding tag.
inline void emit_sh_ljs_bool(a64::Assembler &a, const a64::GpX inOut) {
  static constexpr SHLegacyValue baseBool = HermesValue::encodeBoolValue(false);
  // We know that the ETag for bool has a 0 in its lowest bit, and is therefore
  // a shifted 16 bit value. We can exploit this to use movk to set the tag.
  static_assert(HERMESVALUE_VERSION == 2);
  static_assert(
      (llvh::isShiftedUInt<16, kHV_NumDataBits>(baseBool.raw)),
      "Boolean tag must be 16 bits.");
  a.lsl(inOut, inOut, kHV_BoolBitIdx);
  // Add the bool tag.
  a.movk(inOut, baseBool.raw >> kHV_NumDataBits, kHV_NumDataBits);
}

/// Store the HermesValue for the bool \p val into \p out.
inline void
emit_sh_ljs_bool_const(a64::Assembler &a, const a64::GpX &out, bool val) {
  static constexpr SHLegacyValue baseBool = HermesValue::encodeBoolValue(false);
  // We know that the ETag for bool has a 0 in its lowest bit, and is therefore
  // a shifted 16 bit value. We can exploit this to use movk to set the tag.
  static_assert(HERMESVALUE_VERSION == 2);
  static_assert(
      (llvh::isShiftedUInt<16, kHV_NumDataBits>(baseBool.raw)),
      "Boolean tag must be 16 bits.");
  if (val) {
    // Put 1 at the value and add the bool tag.
    a.mov(out, (uint64_t)1 << kHV_BoolBitIdx);
    a.movk(out, baseBool.raw >> kHV_NumDataBits, kHV_NumDataBits);
  } else {
    a.mov(out, baseBool.raw);
  }
}

/// Maximum natural offset (+1) that can be encoded in a load/store instruction
/// for a given operand width. The instruction contains a 12-bit unsigned
/// offset which is multiplied by the size of the load, so the maximum offset is
/// 2^12 * \c width.
///
/// https://developer.arm.com/documentation/ddi0602/2024-06/Base-Instructions/LDR--immediate---Load-register--immediate--?lang=en
/// https://developer.arm.com/documentation/ddi0602/2024-06/Base-Instructions/LDRH--immediate---Load-register-halfword--immediate--?lang=en
/// https://developer.arm.com/documentation/ddi0602/2024-06/Base-Instructions/LDRB--immediate---Load-register-byte--immediate--?lang=en
constexpr inline uint32_t maxNaturalBaseOffset(unsigned width) {
  return (1 << 12) * width;
}

/// Maximum offset (+1) that can be encoded in a load instruction, i.e. can be
/// loaded by \c emit_load_from_base_offset() without requiring an extra
/// register.
constexpr uint32_t kMaxInlineBaseOffset = maxNaturalBaseOffset(8);

/// Force inlining in release builds only, mirroring JIT_INLINE in JIT.cpp.
/// While every caller shared a translation unit with this helper the compiler
/// inlined it everywhere and emitted no out-of-line copy; once the emitters
/// were split apart it began emitting one per instantiation at -Os, costing
/// around 760 bytes. Debug builds leave the decision to the compiler so the
/// helper stays steppable.
#ifdef NDEBUG
#define JIT_EMIT_ALWAYS_INLINE LLVM_ATTRIBUTE_ALWAYS_INLINE inline
#else
#define JIT_EMIT_ALWAYS_INLINE inline
#endif

/// Load a value with the specified width from the base register with an
/// unsigned immediate offset. The wrapper is necessary since the immediate
/// offset for loads is limited to 12-bits and may not always be sufficient, in
/// which case we need to use alternate techniques.
///
/// \param WIDTH The width of the load, in bytes. Must be one of 1, 2, 4, or 8.
/// \param INLINE_LOAD If true, the offset must be less than or equal to
///   \c kMaxInlineBaseOffset, and the load will always be emitted without
///   needing a temporary register.
/// \param destReg The destination register to load the value into.
/// \param baseReg The base register to calculate the load address.
/// \param tmpReg A temporary register that must be different from \p baseReg,
///   but could be the same as \p destReg. This is only needed when
///   \c INLINE_LOAD is false, pass {} otherwise.
/// \param offset The offset to add to the base register. Must be aligned to
///   WIDTH.
template <unsigned WIDTH, bool INLINE_LOAD = false>
JIT_EMIT_ALWAYS_INLINE void emit_load_from_base_offset(
    a64::Assembler &a,
    const a64::GpX &destReg,
    const a64::GpX &baseReg,
    const a64::GpX &tmpReg,
    uint32_t offset) {
  static_assert(
      WIDTH == 1 || WIDTH == 2 || WIDTH == 4 || WIDTH == 8,
      "Unsupported width for load");
  assert(
      (INLINE_LOAD || tmpReg.isValid()) &&
      "Temporary register must be valid when INLINE_LOAD is false");
  assert(
      (INLINE_LOAD || tmpReg != baseReg) &&
      "tmpReg must not be the same as baseReg when INLINE_LOAD is false");
  // Assert that the offset is aligned to the load width.
  assert(offset % WIDTH == 0 && "Offset must be aligned to the load width");

  // Maximum offset (+1) that can be used when loading a value with this width.
  constexpr uint32_t kMaxNaturalOffset = maxNaturalBaseOffset(WIDTH);

  if (offset < kMaxNaturalOffset) {
    // The offset fits in the natural range for the load width.
    if constexpr (WIDTH == 1) {
      a.ldrb(destReg.w(), a64::Mem(baseReg, offset));
    } else if constexpr (WIDTH == 2) {
      a.ldrh(destReg.w(), a64::Mem(baseReg, offset));
    } else if constexpr (WIDTH == 4) {
      a.ldr(destReg.w(), a64::Mem(baseReg, offset));
    } else {
      a.ldr(destReg, a64::Mem(baseReg, offset));
    }
  } else if (offset < kMaxInlineBaseOffset) {
    // The offset fits in the range for a 64-bit load.
    a.ldr(destReg, a64::Mem(baseReg, offset & ~(uint32_t)7));
    a.ubfx(destReg, destReg, (offset & 7) * 8, WIDTH * 8);
  } else if constexpr (!INLINE_LOAD) {
    // We must perform an explicit addition.
    a.mov(tmpReg, offset / WIDTH);
    if constexpr (WIDTH == 1) {
      a.ldrb(destReg.w(), a64::Mem(baseReg, tmpReg));
    } else if constexpr (WIDTH == 2) {
      a.ldrh(
          destReg.w(),
          a64::Mem(baseReg, tmpReg, a64::Shift(a64::ShiftOp::kLSL, 1)));
    } else if constexpr (WIDTH == 4) {
      a.ldr(
          destReg.w(),
          a64::Mem(baseReg, tmpReg, a64::Shift(a64::ShiftOp::kLSL, 2)));
    } else {
      a.ldr(
          destReg,
          a64::Mem(baseReg, tmpReg, a64::Shift(a64::ShiftOp::kLSL, 3)));
    }
  } else {
    a.reportError(
        asmjit::kErrorInvalidImmediate,
        "emit_load_from_base_offset(): offset is too large for INLINE_LOAD=true");
  }
}

/// For a register containing a pointer to a GCCell, retrieve its CellKind (a
/// single byte) and store it in \p wOut.
/// \p wOut and \p xIn may refer to the same register.
inline void emit_gccell_get_kind(
    a64::Assembler &a,
    const a64::GpX &xOut,
    const a64::GpX &xIn) {
  emit_load_from_base_offset<1, true>(
      a,
      xOut,
      xIn,
      {},
      offsetof(SHGCCell, kindAndSize) + RuntimeOffsets::kindAndSizeKind);
}

/// For a register \p wIn that contains a CellKind, check whether it falls
/// within the kind range [first, last].
/// The \p wInput is not modified unless it is the same as \p wTemp, which is
/// allowed.
/// CPU flags are updated as result. b_ls on success, or b_hi on failure.
inline void emit_cellkind_in_range(
    a64::Assembler &a,
    const a64::GpW &wTemp,
    const a64::GpW &wInput,
    CellKind first,
    CellKind last) {
  a.sub(wTemp, wInput, first);
  a.cmp(wTemp, (uint32_t)last - (uint32_t)first);
}

/// For a register \p dInput, which contains a double, check whether it is a
/// valid signed 64-bit integer.
/// CPU flags are updated. b_eq on success.
/// If successful, \p xTemp will contain the number converted to int,
/// and \p dTemp will contain the same number as \p dInput.
/// \pre dTemp != dInput, because both are used in the comparison.
inline void emit_double_is_int(
    a64::Assembler &a,
    const a64::GpX &xTemp,
    const a64::VecD &dTemp,
    const a64::VecD &dInput) {
  assert(dTemp != dInput && "must use a different temp");

  // Convert the operand to a signed 64 bit integer.
  a.fcvtzs(xTemp, dInput);
  // Sign extend it from the second-to-last bit. This is necessary because
  // fcvtzs is saturating and will convert the double 2^63 to 2^63 - 1, which
  // will get converted back to 2^63 by scvtf. They will therefore incorrectly
  // compare equal after truncation.
  a.sbfx(xTemp, xTemp, 0, 63);
  // Convert back to a double and see if they compare equal.
  a.scvtf(dTemp, xTemp);
  a.fcmp(dTemp, dInput);
}

/// For a register \p dInput, which contains a double, check whether it is a
/// valid unsigned 32-bit integer.
/// CPU flags are updated. b_eq on success.
/// If successful, \p wTemp will contain the number converted to int,
/// and \p dTemp will contain the same number as \p dInput.
/// \pre dTemp != dInput, because both are used in the comparison.
inline void emit_double_is_uint32(
    a64::Assembler &a,
    const a64::GpW &wTemp,
    const a64::VecD &dTemp,
    const a64::VecD &dInput) {
  assert(dTemp != dInput && "must use a different temp");
  a.fcvtzu(wTemp, dInput);
  a.ucvtf(dTemp, wTemp);
  a.fcmp(dTemp, dInput);
}

/// Given a compressed pointer in \p xInOut, decompress it and place the result
/// in \p xInOut.
inline void emit_sh_cp_decode(a64::Assembler &a, const a64::GpX &xInOut) {
#ifdef HERMESVM_COMPRESSED_POINTERS
  a.cmp(xInOut, 0);
  a.add(xInOut, xRuntime, xInOut);
  a.csel(xInOut, a64::xzr, xInOut, a64::CondCode::kEQ);
#endif
}

/// Given a compressed pointer in \p xInOut that is known to be non-null,
/// decompress it and place the result in \p xInOut.
inline void emit_sh_cp_decode_non_null(
    a64::Assembler &a,
    const a64::GpX &xInOut) {
#ifdef HERMESVM_COMPRESSED_POINTERS
  a.add(xInOut, xRuntime, xInOut);
#endif
}

/// Given a pointer in \p xInOut that is known to be non-null, compress it and
/// place the result in \p xInOut.
inline void emit_sh_cp_encode_non_null(
    a64::Assembler &a,
    const a64::GpX &xInOut) {
#ifdef HERMESVM_COMPRESSED_POINTERS
  a.sub(xInOut, xInOut, xRuntime);
#endif
}

/// Similar to \c emit_sh_cp_decode_non_null(), but used when the input register
/// needs to be preserved. It returns the result register which must then be
/// used by the caller.
/// The result register is either \p xIn or \p xMayBeRes.
[[nodiscard]] inline const a64::GpX &emit_sh_cp_decode_non_null_preserve_input(
    a64::Assembler &a,
    const a64::GpX &xMayBeRes,
    const a64::GpX &xIn) {
  if constexpr (sizeof(SmallHermesValue) == 4) {
    a.add(xMayBeRes, xRuntime, xIn);
    return xMayBeRes;
  } else {
    return xIn;
  }
}

/// Load a compressed pointer from \p mem.
inline void
emit_load_cp(a64::Assembler &a, const a64::GpX &dest, const a64::Mem &mem) {
#ifdef HERMESVM_COMPRESSED_POINTERS
  a.ldr(dest.w(), mem);
#else
  a.ldr(dest, mem);
#endif
}

/// Store the compressed pointer in \p val to \p mem.
inline void
emit_store_cp(a64::Assembler &a, const a64::GpX &val, const a64::Mem &mem) {
  if constexpr (sizeof(CompressedPointer) == 4)
    a.str(val.w(), mem);
  else
    a.str(val, mem);
}

/// Load a SmallHermesValue from \p mem.
inline void
emit_load_shv(a64::Assembler &a, const a64::GpX &dest, const a64::Mem &mem) {
  if constexpr (sizeof(SmallHermesValue) == 4) {
    a.ldr(dest.w(), mem);
  } else {
    a.ldr(dest, mem);
  }
}

/// Store a SmallHermesValue to \p mem.
inline asmjit::Error
emit_store_shv(a64::Assembler &a, const a64::GpX &val, const a64::Mem &mem) {
  if constexpr (sizeof(SmallHermesValue) == 4) {
    return a.str(val.w(), mem);
  } else {
    return a.str(val, mem);
  }
}

/// Emit a comparison with an immediate.
inline void emit_cmp_imm32(
    a64::Assembler &a,
    const a64::GpW &wReg,
    uint32_t imm32,
    const a64::GpW &wTmpReg) {
  if (a64::Utils::isAddSubImm(imm32)) {
    a.cmp(wReg, imm32);
  } else {
    a.mov(wTmpReg, imm32);
    a.cmp(wReg, wTmpReg);
  }
}

/// Add an unsigned 24-bit immediate to a register, in place.
/// ADD's immediate is 12 bits, optionally shifted left by 12, so a value that
/// is neither small enough nor a clean multiple of 4096 needs two
/// instructions. Splitting it that way avoids needing a scratch register,
/// which matters at the call-setup sites where every argument register is
/// already spoken for.
inline void
emit_add_imm_u24(a64::Assembler &a, const a64::GpX &xReg, uint32_t imm32) {
  assert(
      imm32 <= 0xFFFFFF &&
      "immediate must fit in 24 bits; ADD has no shift-24 form");
  if (a64::Utils::isAddSubImm(imm32)) {
    a.add(xReg, xReg, imm32);
    return;
  }
  uint32_t hi = imm32 & ~UINT32_C(0xFFF);
  uint32_t lo = imm32 & UINT32_C(0xFFF);
  assert(
      a64::Utils::isAddSubImm(hi) &&
      "high part should encode as a shifted immediate");
  a.add(xReg, xReg, hi);
  if (lo)
    a.add(xReg, xReg, lo);
}

/// Load the slot16 from a cache entry.
/// \param xResReg The register to load the slot16 into.
/// \param xPropCacheReg The register containing the pointer to the start of
///   the read property cache, which must not be the same as \p xResReg.
/// \param cacheIdx The index of the cache entry to load.
inline void emit_load_slot16(
    a64::Assembler &a,
    const a64::GpX &xResReg,
    const a64::GpX &xPropCacheReg,
    uint8_t cacheIdx) {
  assert(
      xPropCacheReg != xResReg &&
      "xPropCacheReg must not be the same as xResReg");
  emit_load_from_base_offset<2>(
      a,
      xResReg,
      xPropCacheReg,
      xResReg,
      sizeof(SHReadPropertyCacheEntry) * cacheIdx +
          offsetof(SHReadPropertyCacheEntry, _slot16));
}

/// A class implementing SmallHermesValue to HermesValue decoding.
/// Given a SmallHermesValue in \p xInOut, decompress it and place the result in
/// \p xInOut. Jump to \p doneLab if decoding is done early (but not in the
/// fall-through case).
///
/// To facilitate sharing the decoding without an extra branch, the code is
/// split into two parts: the first case and the rest of the cases. The first
/// case checks its condition: if it doesn't match, it jumps to the rest of the
/// cases. If it matches, it performs the decode and falls through.
/// The intent is that the "rest cases" will be shared, while the first case
/// may be emitted multiple times to save a branch.
class Emit_sh_shv_decode {
#ifdef HERMESVM_BOXED_DOUBLES
  const a64::GpX xInOut;
  asmjit::Label ptrLab;
  const asmjit::Label &doneLab;
#endif
#ifndef NDEBUG
  bool restEmitted = false;
#endif

 public:
  explicit Emit_sh_shv_decode(
      a64::Assembler &a,
      const a64::GpX &xInOut,
      const asmjit::Label &doneLab)
#ifdef HERMESVM_BOXED_DOUBLES
      : xInOut(xInOut), doneLab(doneLab) {
    ptrLab = a.newLabel();
  }
#else
  {
  }
#endif

  /// Emit the code checking and decoding the first case of SHV values. Can be
  /// invoked multiple times. Emitted code looks like:
  /// \code
  ///    check
  ///    b.cond ptrLab
  ///    decode
  /// \endcode
  ///
  /// Note that it simply falls through on success.
  void emitFirstCase(a64::Assembler &a);

  /// Emit the code checking and decoding all other cases of SHV values. Can
  /// only be invoked once. Emitted code at high level looks like:
  /// \code
  ///    ptrLab:
  ///      check_ptr
  ///      b.cond otherLab
  ///      decode
  ///      b doneLab
  ///    otherLab:
  ///      decode
  /// \endcode
  ///
  /// Note that the last case does not branch to doneLab, so the caller must
  /// do it (or fall through to doneLab).
  void emitRestCases(a64::Assembler &a);

  /// Emit the first case, followed by a branch to doneLab, and the rest of the
  /// cases.
  void emitAll(a64::Assembler &a) {
    emitFirstCase(a);
#ifdef HERMESVM_BOXED_DOUBLES
    // emitFirstCase() falls through, so add an explicit branch to doneLab.
    a.b(doneLab);
#endif
    emitRestCases(a);
  }
};

/// Given a SmallHermesValue in \p xInOut, decompress it into a HermesValue and
/// place the result in \p xInOut. Jump to \p doneLab if decoding is done early
/// (but not in the fall-through case).
inline void emit_sh_shv_decode(
    a64::Assembler &a,
    const a64::GpX &xInOut,
    const asmjit::Label &doneLab) {
  Emit_sh_shv_decode(a, xInOut, doneLab).emitAll(a);
}

/// \return true if i is a valid immediate offset to use in an stp instruction
/// storing two GpX registers, false otherwise. Note that the limits are
/// different if storing GpW registers, or vector registers.
inline bool isStpGpXImm(int i) {
  // These restrictions are from:
  // https://developer.arm.com/documentation/ddi0602/2025-03/Base-Instructions/STP--Store-pair-of-registers-
  return (i % 8 == 0) && i <= 504 && i >= -512;
}

/// This macro is used to catch and handle low probability instructing encoding
/// errors - i.e. when an immediate operand doesn't fit in the instruction
/// encoding. It causes Asmjit to just return an error code instead of
/// terminating the entire compilation.
///
/// \param expValue the error value that we want to handle.
/// \param code  C++ code to invoke asmjit and store the result in a variable.
#define EXPECT_ERROR(expValue, code)          \
  do {                                        \
    assert(                                   \
        expectedError_ == asmjit::kErrorOk && \
        "expectedError_ is not cleared");     \
    expectedError_ = (expValue);              \
    code;                                     \
    expectedError_ = asmjit::kErrorOk;        \
  } while (0)

/// Save the current IP and emit a call to a runtime function. This should be
/// used in most cases when invoking slow paths and handlers for complex
/// functionality.
#define EMIT_RUNTIME_CALL(em, type, func)             \
  do {                                                \
    using _FnT = type;                                \
    _FnT _fn = func;                                  \
    (void)_fn;                                        \
    (em).callRuntimeWithSavedIP((void *)func, #func); \
  } while (0)

/// Call a runtime function without saving the IP. This is intended for special
/// cases where we want to preserve the currently saved IP or if the IP is not
/// needed.
#define EMIT_RUNTIME_CALL_WITHOUT_SAVED_IP(em, type, func) \
  do {                                                     \
    using _FnT = type;                                     \
    _FnT _fn = func;                                       \
    (void)_fn;                                             \
    (em).callRuntime((void *)func, #func);                 \
  } while (0)

/// Load the lengthAndFlags of a HermesValue that contains a StringPrimitive
/// into \p xOut.
/// xOut and xIn may be the same, but xIn will be clobbered.
/// \param xOut the output register for the length, which will be placed
///  in xOut.w().
/// \param xIn the input register containing the string HermesValue.
void emit_stringprim_get_length_and_flags(
    a64::Assembler &a,
    const a64::GpX &xOut,
    const a64::GpX &xIn);

/// Emit code to initialize the fields on a JSObject.
/// \param xObj contains a pointer to the object to initialise.
/// \param xTempOrPropStorageOpt is a temporary register. It may contain
///   a pointer to the PropStorage. If \p HasPropStorage is true, it's
///   used to initialize the PropStorage, otherwise it's used as a temporary
///   register.
///   Either way, the value in xTempOrPropStorageOpt WILL be overwritten.
/// \param xClazzOpt if invalid will use the default JSObject HiddenClass,
///   otherwise a compressed pointer to the HiddenClass of the new object.
void emit_jsobject_init(
    a64::Assembler &a,
    const a64::GpX &xObj,
    const a64::GpX &xTempOrPropStorageOpt,
    bool hasPropStorage,
    const a64::GpX &xClazzOpt = a64::GpX{});

/// Emit code to initialize the fields of a newly created environment.
/// \param xNewEnvPtr contains a pointer to the object to initialise.
/// \param xParentEnv contains a compressed pointer to the parent environment.
/// \param xTemp is a temporary register for use by the emitted code.
/// \param vTemp is a temporary vector register for use by the emitted code.
/// \param size is the number of slots in the new environment.
void emit_environment_init(
    a64::Assembler &a,
    const a64::GpX &xNewEnvPtr,
    const a64::GpX &xParentEnv,
    const a64::GpX &xTemp,
    const a64::VecV &vTemp,
    uint32_t size);

class OurErrorHandler : public asmjit::ErrorHandler {
  asmjit::Error &expectedError_;
  std::function<void(std::string &&message)> const longjmpError_;

 public:
  /// \param expectedError if we get an error matching this value, we ignore it.
  explicit OurErrorHandler(
      asmjit::Error &expectedError,
      const std::function<void(std::string &&message)> &longjmpError)
      : expectedError_(expectedError), longjmpError_(longjmpError) {}

  void handleError(
      asmjit::Error err,
      const char *message,
      asmjit::BaseEmitter *origin) override;
};

/// Helper function to load a pointer to the builtin closure with index
/// \p builtinIndex and place it in \p xRes.
void emit_load_builtin_closure(
    a64::Assembler &a,
    const a64::GpX &xRes,
    uint32_t builtinIndex);

#ifndef ASMJIT_NO_LOGGING
class OurLogger : public asmjit::Logger {
 private:
  a64::Assembler &a_;
  PerfJitDump *perfJitDump_{nullptr};
  bool dumpJitCode_{false};

 public:
  OurLogger(a64::Assembler &a, PerfJitDump *perfJitDump, bool dumpJitCode)
      : a_(a), perfJitDump_(perfJitDump), dumpJitCode_(dumpJitCode) {}

  ASMJIT_API asmjit::Error _log(const char *data, size_t size) noexcept
      override;
};
#endif

} // namespace hermes::vm::arm64

#endif // HERMESVM_JIT

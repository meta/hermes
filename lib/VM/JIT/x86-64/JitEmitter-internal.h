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

namespace hermes::vm::x86_64 {

// Ensure that HermesValue tags are handled correctly by updating this every
// time the HERMESVALUE_VERSION changes, and going through the JIT and updating
// any relevant code.
static_assert(
    HERMESVALUE_VERSION == 2,
    "HermesValue version mismatch, JIT may need to be updated");

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

/// Get the tag for the HermesValue in \p in and place it in \p out.
///
/// x86-64: the shift is two-operand, so unlike arm64 \p out is always
/// written, and \p in is preserved only when the two differ.
inline void
emit_sh_ljs_get_tag(x86::Assembler &a, const x86::Gp &out, const x86::Gp &in) {
  static_assert(
      HERMESVALUE_VERSION == 2,
      "kHV_NumDataBits is 48 and can be easily shifted");
  if (out != in)
    a.mov(out, in);
  a.sar(out, kHV_NumDataBits);
}

/// Check whether \p tagReg is the tag for a pointer value.
/// CPU flags are updated as result. jae on success.
///
/// x86-64: arm64 has to phrase the comparison as `cmn tag, -tag`, since its
/// immediates are unsigned; x86's imm32 is sign-extended, so the negative
/// tag is a plain `cmp` operand.
inline void emit_sh_ljs_tag_is_pointer(
    x86::Assembler &a,
    const x86::Gp &tagReg) {
  static_assert(
      HERMESVALUE_VERSION == 2,
      "All tags above HVTag_FirstPointer are pointers");
  // The valid pointer tags are: 0xfd (HVTag_FirstPointer) to 0xff
  // (HVTag_Last), but all sign extended to 64 bits.
  // We need an unsigned comparison (tag >= 0xfd) to catch all pointers.
  // Doubles may have 0 in the tag bits, e.g. so we have to use
  // unsigned condition codes to make sure they don't get detected as pointers.
  a.cmp(tagReg, asmjit::Imm(HVTag_FirstPointer));
}

/// Emit code to check whether \p tagReg is an object tag.
/// The input reg is not modified.
/// CPU flags are updated as result. je on success.
inline void emit_sh_ljs_tag_is_object(
    x86::Assembler &a,
    const x86::Gp &tagReg) {
  static_assert(
      (int16_t)HVTag_Object == (int16_t)(-1), "HVTag_Object must be -1");
  a.cmp(tagReg, asmjit::Imm(HVTag_Object));
}

/// Emit code to check whether \p tagReg is a string tag.
/// The input reg is not modified.
/// CPU flags are updated as result. je on success.
inline void emit_sh_ljs_tag_is_string(
    x86::Assembler &a,
    const x86::Gp &tagReg) {
  static_assert((int16_t)HVTag_Str == (int16_t)(-3), "HVTag_Str must be -3");
  a.cmp(tagReg, asmjit::Imm(HVTag_Str));
}

/// Emit code to check whether the input reg is an object, using the
/// specified temp register. The input reg is not modified unless it is
/// the same as the temp, which is allowed.
/// CPU flags are updated as result. je on success.
inline void emit_sh_ljs_is_object(
    x86::Assembler &a,
    const x86::Gp &tempReg,
    const x86::Gp &inputReg) {
  emit_sh_ljs_get_tag(a, tempReg, inputReg);
  emit_sh_ljs_tag_is_object(a, tempReg);
}

/// Emit code to check whether the input reg is a string, using the specified
/// temp register. The input reg is not modified unless it is the same as the
/// temp, which is allowed.
/// CPU flags are updated as result. je on success.
inline void emit_sh_ljs_is_string(
    x86::Assembler &a,
    const x86::Gp &tempReg,
    const x86::Gp &inputReg) {
  emit_sh_ljs_get_tag(a, tempReg, inputReg);
  emit_sh_ljs_tag_is_string(a, tempReg);
}

/// Emit code to check whether the input reg is a bigint, using the specified
/// temp register. The input reg is not modified unless it is the same as the
/// temp, which is allowed.
/// CPU flags are updated as result. je on success.
///
/// x86-64: as everywhere in this file, arm64's cmn-with-negation is a plain
/// cmp here, because an x86 imm32 is sign-extended.
inline void emit_sh_ljs_is_bigint(
    x86::Assembler &a,
    const x86::Gp &tempReg,
    const x86::Gp &inputReg) {
  static_assert(
      (int16_t)HVTag_BigInt == (int16_t)(-2), "HVTag_BigInt must be -2");
  emit_sh_ljs_get_tag(a, tempReg, inputReg);
  a.cmp(tempReg, asmjit::Imm(HVTag_BigInt));
}

/// Emit code to check whether the input reg is a symbol, using the specified
/// temp register. The input reg is not modified unless it is the same as the
/// temp, which is allowed.
/// CPU flags are updated as result. je on success.
///
/// x86-64: shaped exactly like emit_sh_ljs_is_bool() -- the ETag is read by
/// shifting one bit further than the tag, and the two-operand shift needs a
/// copy when the registers differ.
inline void emit_sh_ljs_is_symbol(
    x86::Assembler &a,
    const x86::Gp &tempReg,
    const x86::Gp &inputReg) {
  static_assert(
      HERMESVALUE_VERSION == 2, "HVETag_Symbol must be at kHV_NumDataBits - 1");
  static_assert(
      (int16_t)HVETag_Symbol == (int16_t)(-9), "HVETag_Symbol must be -9");
  if (tempReg != inputReg)
    a.mov(tempReg, inputReg);
  a.sar(tempReg, kHV_NumDataBits - 1);
  a.cmp(tempReg, asmjit::Imm(HVETag_Symbol));
}

/// Extract the pointer out of the HermesValue in \p in into \p out.
///
/// x86-64: kHV_DataMask does not fit in a sign-extended imm32 and this
/// helper has no scratch register, so the tag is shifted out and back in
/// instead of masked off with arm64's single `and`.
inline void emit_sh_ljs_get_pointer(
    x86::Assembler &a,
    const x86::Gp &out,
    const x86::Gp &in) {
  static_assert(
      HERMESVALUE_VERSION == 2,
      "kHV_DataMask is 0x000...1111... and is exactly the low kHV_NumDataBits");
  if (out != in)
    a.mov(out, in);
  a.shl(out, 64 - kHV_NumDataBits);
  a.shr(out, 64 - kHV_NumDataBits);
}

/// Encode a native pointer into a tagged object pointer (SHLegacyValue).
/// The same register is used for input and output.
///
/// x86-64: arm64 folds the tag in with `movk`, which x86 has no equivalent
/// of: the only 16-bit subregister x86 can write is the low one, and the tag
/// constant is far too wide for a sign-extended imm32. Rather than
/// materializing it in xScratch, the top 16 bits are rotated down to the
/// bottom, where the tag does fit an imm32, and rotated back, which leaves
/// xScratch untouched for the callers. The input is a raw heap pointer, whose
/// top 16 bits are zero, which is what makes `or` equivalent to arm64's `movk`.
///
/// All three instructions write EFLAGS, where arm64's single `movk` writes no
/// condition flags at all, so a caller must not hold live flags across this
/// helper. The same applies to emit_sh_ljs_object2() below, which ends in it.
inline void emit_sh_ljs_object(x86::Assembler &a, const x86::Gp &inOut) {
  static_assert(
      HERMESVALUE_VERSION == 2,
      "HVTag_Object << kHV_NumDataBits is 0x1111...0000...");
  constexpr unsigned kTagBits = 64 - kHV_NumDataBits;
  static_assert(kTagBits == 16, "the tag must be exactly the top 16 bits");
  a.rol(inOut, kTagBits);
  a.or_(inOut, asmjit::Imm((uint16_t)HVTag_Object));
  a.ror(inOut, kTagBits);
}

/// Encode a native pointer into a tagged object pointer (SHLegacyValue).
/// Takes an input and output register, which can be the same.
///
/// x86-64: arm64 gets the extra register for free, since its `orr` is
/// three-operand; here it costs a move.
inline void
emit_sh_ljs_object2(x86::Assembler &a, const x86::Gp &out, const x86::Gp &in) {
  if (out != in)
    a.mov(out, in);
  emit_sh_ljs_object(a, out);
}

/// Encode a string pointer into a tagged string (SHLegacyValue).
/// The same register is used for input and output.
///
/// x86-64: the rotate-or-rotate shape and its preconditions are exactly
/// emit_sh_ljs_object()'s -- see the comment there, including the note that
/// all three instructions write EFLAGS where arm64's single `movk` writes
/// none.
inline void emit_sh_ljs_string(x86::Assembler &a, const x86::Gp &inOut) {
  static_assert(
      HERMESVALUE_VERSION == 2,
      "HVTag_Str << kHV_NumDataBits is 0x1101...0000...");
  constexpr unsigned kTagBits = 64 - kHV_NumDataBits;
  static_assert(kTagBits == 16, "the tag must be exactly the top 16 bits");
  a.rol(inOut, kTagBits);
  a.or_(inOut, asmjit::Imm((uint16_t)HVTag_Str));
  a.ror(inOut, kTagBits);
}

/// Encode a string pointer into a tagged string (SmallHermesValue).
/// The same register is used for input and output.
///
/// x86-64: in the boxed-doubles configuration the tag is a small constant
/// that fits a sign-extended imm32, so the `or` takes no scratch register.
/// That branch is compiled and executed by both boxed builds
/// (HEAP_HV_PREFER32 and HEAP_HV_BOXED); it is what tags the string values
/// an object-literal buffer writes. See emit_sh_cp_decode_non_null().
inline void emit_shv_string(x86::Assembler &a, const x86::Gp &inOut) {
#ifdef HERMESVM_BOXED_DOUBLES
  static_assert(
      SmallHermesValue::kVersion == 1, "String tagging requires simple or");
  a.or_(inOut, asmjit::Imm((uint32_t)HermesValue32::Tag::String));
#else
  emit_sh_ljs_string(a, inOut);
#endif
}

/// Emit code to check whether the input reg is empty, using the specified
/// temp register. The input reg is not modified unless it is the same as the
/// temp, which is allowed.
/// CPU flags are updated as result. je on success.
///
/// x86-64: shaped exactly like emit_sh_ljs_is_null() below -- empty is an
/// ETag, so the shift goes one bit further than the tag, the two-operand
/// shift needs a copy when the registers differ, and the sign-extended imm32
/// makes arm64's cmn-with-negation unnecessary.
inline void emit_sh_ljs_is_empty(
    x86::Assembler &a,
    const x86::Gp &tempReg,
    const x86::Gp &inputReg) {
  // Get the ETag bits by right shifting one bit further than the tag.
  static_assert(
      (int16_t)HVETag_Empty == (int16_t)(-14), "HVETag_Empty must be -14");
  if (tempReg != inputReg)
    a.mov(tempReg, inputReg);
  a.sar(tempReg, kHV_NumDataBits - 1);
  a.cmp(tempReg, asmjit::Imm(HVETag_Empty));
}

/// Emit code to check whether the input reg is null, using the specified
/// temp register. The input reg is not modified unless it is the same as the
/// temp, which is allowed.
/// CPU flags are updated as result. je on success.
///
/// x86-64: shaped exactly like emit_sh_ljs_is_bool() below -- the
/// two-operand shift needs a copy when the registers differ, and the
/// sign-extended imm32 makes arm64's cmn-with-negation unnecessary.
inline void emit_sh_ljs_is_null(
    x86::Assembler &a,
    const x86::Gp &tempReg,
    const x86::Gp &inputReg) {
  // Get the ETag bits by right shifting one bit further than the tag.
  static_assert(
      (int16_t)HVETag_Null == (int16_t)(-11), "HVETag_Null must be -11");
  if (tempReg != inputReg)
    a.mov(tempReg, inputReg);
  a.sar(tempReg, kHV_NumDataBits - 1);
  a.cmp(tempReg, asmjit::Imm(HVETag_Null));
}

/// Given a compressed pointer in \p inOut that is known to be non-null,
/// decompress it and place the result in \p inOut.
///
/// x86-64: the compressed-pointer branch below is transcribed from arm64.
/// It is compiled and executed by the HEAP_HV_PREFER32 build, which is the
/// only x86-64 configuration that defines HERMESVM_COMPRESSED_POINTERS --
/// HEAP_HV_BOXED sets only HERMESVM_BOXED_DOUBLES, so the branch is inert
/// there just as it is in HEAP_HV_64. This is the reference note for the
/// cp helpers below: each of them is live in HV32 and inert in the other
/// two builds. See doc/JIT.md's heap-value-mode build matrix.
///
/// x86-64: `add` also writes EFLAGS, where arm64's three-operand `add` does
/// not touch flags at all, so a caller must not hold live flags across this
/// helper.
inline void emit_sh_cp_decode_non_null(
    x86::Assembler &a,
    const x86::Gp &inOut) {
#ifdef HERMESVM_COMPRESSED_POINTERS
  a.add(inOut, xRuntime);
#endif
}

/// Given a compressed pointer in \p inOut, which may be null, decompress it
/// and place the result in \p inOut. A null input decodes to a null pointer
/// rather than to the heap base.
///
/// x86-64: arm64 selects between the sum and zero with a csel against its
/// hardwired zero register. x86 has no such register, so \p zeroTemp -- which
/// must differ from \p inOut -- is zeroed and used as the cmov source. This
/// helper therefore takes a parameter arm64's does not. The add is written as
/// an lea precisely because lea does not write EFLAGS, which lets the `test`
/// that reads the original value sit before it and still control the cmov.
///
/// See emit_sh_cp_decode_non_null() -- the branch below is live in the HV32
/// build, so \p zeroTemp is used there and unused in the HV64 and BOXED
/// builds. Its caller is loadParentNoTraps, which `super.m()` emits.
inline void emit_sh_cp_decode(
    x86::Assembler &a,
    const x86::Gp &inOut,
    [[maybe_unused]] const x86::Gp &zeroTemp) {
#ifdef HERMESVM_COMPRESSED_POINTERS
  assert(zeroTemp != inOut && "zero temp must differ from the input");
  a.mov(zeroTemp, asmjit::Imm(0));
  a.test(inOut, inOut);
  a.lea(inOut, x86::ptr(inOut, xRuntime));
  a.cmovz(inOut, zeroTemp);
#endif
}

/// Similar to \c emit_sh_cp_decode_non_null(), but used when the input
/// register needs to be preserved. It returns the result register which must
/// then be used by the caller.
/// The result register is either \p in or \p mayBeRes.
///
/// x86-64: arm64's three-operand `add` becomes an `lea`, which computes the
/// same sum into a third register and, unlike `add`, writes no EFLAGS.
///
/// See emit_sh_cp_decode_non_null() -- the narrow branch below is live in
/// the HV32 build, so \p mayBeRes is used there and unused in the HV64 and
/// BOXED builds. Its only caller is the GetById object-specialization tier,
/// which is emitted only once a call site's cache has warmed up; under
/// -Xjit=force nothing warms, so props.js's threshold-mode SPEC RUN lines
/// are the only coverage this helper has. Do not delete them.
[[nodiscard]] inline const x86::Gp &emit_sh_cp_decode_non_null_preserve_input(
    x86::Assembler &a,
    [[maybe_unused]] const x86::Gp &mayBeRes,
    const x86::Gp &in) {
  // sizeof(SmallHermesValue) == 4 holds exactly when pointers are compressed:
  // the only build state with a 4-byte SmallHermesValue also sets
  // HERMESVM_COMPRESSED_POINTERS (see the HEAP_HV_MODE table in
  // CMakeLists.txt). This is arm64's condition, kept verbatim.
  if constexpr (sizeof(SmallHermesValue) == 4) {
    a.lea(mayBeRes, x86::ptr(xRuntime, in));
    return mayBeRes;
  } else {
    return in;
  }
}

/// Given a pointer in \p inOut that is known to be non-null, compress it and
/// place the result in \p inOut.
///
/// x86-64: see emit_sh_cp_decode_non_null() -- the compressed-pointer branch
/// is live in the HV32 build and inert in the other two.
///
/// x86-64: `sub` also writes EFLAGS, where arm64's three-operand `sub` does
/// not touch flags at all, so a caller must not hold live flags across this
/// helper.
inline void emit_sh_cp_encode_non_null(
    x86::Assembler &a,
    const x86::Gp &inOut) {
#ifdef HERMESVM_COMPRESSED_POINTERS
  a.sub(inOut, xRuntime);
#endif
}

/// Load a compressed pointer from \p mem into \p dest.
///
/// x86-64: an x86::Mem carries its own operand size, and the callers build
/// theirs with the size-less x86::ptr(), so each branch stamps the width it
/// needs onto a copy. The 32-bit load zero-extends into the full register,
/// which is what makes the narrow branch a decompressible value.
/// See emit_sh_cp_decode_non_null() -- the narrow branch is live in the HV32
/// build and inert in the other two.
///
/// x86-64: unlike its neighbors above, `mov` is flag-clean, so this narrow
/// branch leaves no EFLAGS divergence for a caller to worry about.
inline void
emit_load_cp(x86::Assembler &a, const x86::Gp &dest, const x86::Mem &mem) {
  x86::Mem m = mem;
#ifdef HERMESVM_COMPRESSED_POINTERS
  m.setSize(4);
  a.mov(dest.r32(), m);
#else
  m.setSize(8);
  a.mov(dest, m);
#endif
}

/// Store the compressed pointer in \p val to \p mem.
///
/// x86-64: see emit_load_cp() for the operand-size handling. As on arm64 the
/// narrow case here is selected with `if constexpr` on the size rather than
/// with the preprocessor, so both halves are always compiled.
inline void
emit_store_cp(x86::Assembler &a, const x86::Gp &val, const x86::Mem &mem) {
  x86::Mem m = mem;
  if constexpr (sizeof(CompressedPointer) == 4) {
    m.setSize(4);
    a.mov(m, val.r32());
  } else {
    m.setSize(8);
    a.mov(m, val);
  }
}

/// Load a SmallHermesValue from \p mem into \p dest.
///
/// x86-64: see emit_load_cp() for the operand-size handling. The narrow load
/// writes the 32-bit destination, which zeroes the upper half of \p dest --
/// exactly what the decoder below expects to see.
inline void
emit_load_shv(x86::Assembler &a, const x86::Gp &dest, const x86::Mem &mem) {
  x86::Mem m = mem;
  if constexpr (sizeof(SmallHermesValue) == 4) {
    m.setSize(4);
    a.mov(dest.r32(), m);
  } else {
    m.setSize(8);
    a.mov(dest, m);
  }
}

/// Store a SmallHermesValue held in \p val to \p mem.
///
/// x86-64: see emit_load_cp() for the operand-size handling. arm64's version
/// returns an asmjit::Error because its scaled store immediate runs out of
/// range for a large enough slot index and the caller has to retry through a
/// register offset; every x86 displacement is a signed 32-bit one, so this
/// always encodes and returns nothing.
inline void
emit_store_shv(x86::Assembler &a, const x86::Gp &val, const x86::Mem &mem) {
  x86::Mem m = mem;
  if constexpr (sizeof(SmallHermesValue) == 4) {
    m.setSize(4);
    a.mov(m, val.r32());
  } else {
    m.setSize(8);
    a.mov(m, val);
  }
}

/// Load the slot16 from a read property cache entry.
/// \param resReg The register to load the slot16 into. It is written as a
///   32-bit register, so its upper half ends up zero and it is usable as a
///   scaled index.
/// \param propCacheReg The register containing the pointer to the start of
///   the read property cache.
/// \param cacheIdx The index of the cache entry to load.
///
/// x86-64: arm64 routes this through emit_load_from_base_offset and therefore
/// needs \p resReg to differ from \p propCacheReg, since that helper may use
/// the destination as a scratch. Here it is a single movzx off a signed
/// 32-bit displacement, so the two registers may alias; the callers keep them
/// distinct anyway, matching arm64.
inline void emit_load_slot16(
    x86::Assembler &a,
    const x86::Gp &resReg,
    const x86::Gp &propCacheReg,
    uint8_t cacheIdx) {
  size_t ofs = sizeof(SHReadPropertyCacheEntry) * cacheIdx +
      offsetof(SHReadPropertyCacheEntry, _slot16);
  assert(ofs <= (size_t)INT32_MAX && "cache entry offset must fit a disp32");
  a.movzx(resReg.r32(), x86::word_ptr(propCacheReg, (int32_t)ofs));
}

/// A class implementing SmallHermesValue to HermesValue decoding.
/// Given a SmallHermesValue in \p inOut, decompress it and place the result in
/// \p inOut. Jump to \p doneLab if decoding is done early (but not in the
/// fall-through case).
///
/// To facilitate sharing the decoding without an extra branch, the code is
/// split into two parts: the first case and the rest of the cases. The first
/// case checks its condition: if it doesn't match, it jumps to the rest of the
/// cases. If it matches, it performs the decode and falls through.
/// The intent is that the "rest cases" will be shared, while the first case
/// may be emitted multiple times to save a branch.
///
/// x86-64: in the boxed-doubles configuration the emitted code clobbers
/// xScratch, where arm64 needs no scratch at all (it composes the HermesValue
/// tag with movk/bfi, which x86 has no equivalent of). Callers must therefore
/// not hold a live value in xScratch across emitFirstCase()/emitRestCases().
/// That is the same rule every other transient user of xScratch follows, and
/// both call sites in this backend -- the GetById inline cache and
/// getOwnBySlotIdx -- are straight-line code with no call in sight.
/// In the default HV64 build nothing at all is emitted and the class is empty.
class Emit_sh_shv_decode {
#ifdef HERMESVM_BOXED_DOUBLES
  const x86::Gp inOut;
  asmjit::Label ptrLab;
  const asmjit::Label &doneLab;
#endif
#ifndef NDEBUG
  bool restEmitted = false;
#endif

 public:
  explicit Emit_sh_shv_decode(
      [[maybe_unused]] x86::Assembler &a,
      [[maybe_unused]] const x86::Gp &inOut,
      [[maybe_unused]] const asmjit::Label &doneLab)
#ifdef HERMESVM_BOXED_DOUBLES
      : inOut(inOut), doneLab(doneLab) {
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
  ///    jcc ptrLab
  ///    decode
  /// \endcode
  ///
  /// Note that it simply falls through on success.
  void emitFirstCase(x86::Assembler &a);

  /// Emit the code checking and decoding all other cases of SHV values. Can
  /// only be invoked once. Emitted code at high level looks like:
  /// \code
  ///    ptrLab:
  ///      check_ptr
  ///      jcc otherLab
  ///      decode
  ///      jmp doneLab
  ///    otherLab:
  ///      decode
  /// \endcode
  ///
  /// Note that the last case does not branch to doneLab, so the caller must
  /// do it (or fall through to doneLab).
  void emitRestCases(x86::Assembler &a);

  /// Emit the first case, followed by a branch to doneLab, and the rest of the
  /// cases.
  void emitAll(x86::Assembler &a) {
    emitFirstCase(a);
#ifdef HERMESVM_BOXED_DOUBLES
    // emitFirstCase() falls through, so add an explicit branch to doneLab.
    a.jmp(doneLab);
#endif
    emitRestCases(a);
  }
};

/// Given a SmallHermesValue in \p inOut, decompress it into a HermesValue and
/// place the result in \p inOut. Jump to \p doneLab if decoding is done early
/// (but not in the fall-through case).
inline void emit_sh_shv_decode(
    x86::Assembler &a,
    const x86::Gp &inOut,
    const asmjit::Label &doneLab) {
  Emit_sh_shv_decode(a, inOut, doneLab).emitAll(a);
}

/// For a register containing a pointer to a GCCell, retrieve its CellKind (a
/// single byte) and place it in \p out.
/// \p out and \p in may refer to the same register.
///
/// x86-64: arm64 routes this through emit_load_from_base_offset because its
/// byte load has a limited immediate range; every x86 displacement is a
/// signed 32-bit one, so this is a single movzx. The 32-bit destination
/// zeroes the upper half of \p out, which is what lets
/// emit_cellkind_in_range() and the jitCallArray index treat it as a plain
/// unsigned value.
inline void
emit_gccell_get_kind(x86::Assembler &a, const x86::Gp &out, const x86::Gp &in) {
  a.movzx(
      out.r32(),
      x86::byte_ptr(
          in.r64(),
          (int32_t)(offsetof(SHGCCell, kindAndSize) +
                    RuntimeOffsets::kindAndSizeKind)));
}

/// For a register \p input that contains a CellKind, check whether it falls
/// within the kind range [first, last].
/// \p input is not modified unless it is the same as \p temp, which is
/// allowed.
/// CPU flags are updated as a result: jbe on success, or ja on failure.
///
/// x86-64: the subtract-and-unsigned-compare trick is arm64's, but the
/// subtraction has to be non-destructive and x86's sub is two-operand, so it
/// is done with an lea. \p input's upper 32 bits are zero (see
/// emit_gccell_get_kind), so computing the address in 64 bits and keeping
/// the low 32 is exactly the 32-bit subtraction arm64 performs.
inline void emit_cellkind_in_range(
    x86::Assembler &a,
    const x86::Gp &temp,
    const x86::Gp &input,
    CellKind first,
    CellKind last) {
  if ((uint32_t)first != 0)
    a.lea(temp.r32(), x86::ptr(input.r64(), -(int32_t)(uint32_t)first));
  else if (temp != input)
    a.mov(temp.r32(), input.r32());
  a.cmp(temp.r32(), asmjit::Imm((uint32_t)last - (uint32_t)first));
}

/// Emit code to check whether the input HermesValue is a double.
/// The input reg is not modified.
/// The temp reg is modified.
/// CPU flags are updated as result. jb on success.
inline void emit_sh_ljs_is_double(
    x86::Assembler &a,
    const x86::Gp &input,
    const x86::Gp &tmp) {
  static_assert(
      HERMESVALUE_VERSION == 2,
      "numbers must be lower than HVTag_First << kHV_NumDataBits");
  a.mov(tmp, asmjit::Imm((uint64_t)HVTag_First << kHV_NumDataBits));
  a.cmp(input, tmp);
}

/// Emit code to check whether the input reg is bool, using the specified
/// temp register.
/// The input reg is not modified unless it is the same as the temp,
/// which is allowed.
/// CPU flags are updated as result. je on success.
///
/// x86-64: the two-operand shift needs a copy when tempReg and inputReg
/// differ, unlike arm64's three-operand asr; when they are the same
/// register the shift simply runs in place, exactly as arm64 allows.
inline void emit_sh_ljs_is_bool(
    x86::Assembler &a,
    const x86::Gp &tempReg,
    const x86::Gp &inputReg) {
  // Get the ETag bits by right shifting one bit further than the tag, and
  // compare against the sign-extended ETag constant directly (imm32 is
  // sign-extended on x86, so there is no arm64-style cmn-with-negation).
  static_assert(
      (int16_t)HVETag_Bool == (int16_t)(-10), "HVETag_Bool must be -10");
  if (tempReg != inputReg)
    a.mov(tempReg, inputReg);
  a.sar(tempReg, kHV_NumDataBits - 1);
  a.cmp(tempReg, asmjit::Imm(HVETag_Bool));
}

/// For a register \p dInput, which contains a double, check whether it is
/// exactly an integer, i.e. whether truncating it and converting back gives
/// the same value.
/// CPU flags are updated: the value is an integer only if both \c jne and
/// \c jp fall through (see below).
/// If successful, \p xTemp will contain the number converted to a signed 64
/// bit integer; \p dTemp is clobbered either way.
/// \pre dTemp != dInput, because both are used in the comparison.
///
/// x86-64: arm64's fcvtzs saturates, so it has to defeat the saturation with
/// an sbfx before the round trip, or the doubles 2^63 and -2^63 would both
/// convert back to themselves and be accepted with the wrong low 32 bits.
/// vcvttsd2si does not saturate: every out-of-range input, and every NaN,
/// produces the "integer indefinite" value INT64_MIN, which converts back to
/// exactly -2^63. The only input that value can compare equal to is -2^63
/// itself, whose conversion to INT64_MIN is the correct one -- its low 32
/// bits are 0, which is ToInt32(-2^63). So no sbfx analogue is needed here
/// and the round-trip compare alone is exact.
///
/// The other divergence is the branch. arm64's fcmp leaves NE set on
/// unordered, so a single b.ne covers "different" and "input was a NaN".
/// vucomisd instead reports unordered as ZF=PF=CF=1, i.e. as *equal*, so the
/// caller must branch on both: jne for an ordered mismatch and jp for the
/// unordered case.
inline void emit_double_is_int(
    x86::Assembler &a,
    const x86::Gp &xTemp,
    const x86::Xmm &dTemp,
    const x86::Xmm &dInput) {
  assert(dTemp != dInput && "must use a different temp");
  assert(xTemp.isGpq() && "the round trip must go through 64 bits");

  // Convert the operand to a signed 64 bit integer.
  a.vcvttsd2si(xTemp, dInput);
  // Convert back to a double and see if they compare equal. dTemp is also
  // the source of the untouched upper half, which is dead here.
  a.vcvtsi2sd(dTemp, dTemp, xTemp);
  a.vucomisd(dTemp, dInput);
}

/// For a register \p dInput, which contains a double, check whether it is a
/// valid unsigned 32-bit integer.
/// CPU flags are updated: the value is a uint32 only if both \c jne and \c jp
/// fall through (see below).
/// If successful, the low 32 bits of \p xTemp contain the converted index and
/// its upper 32 bits are zero, so it is directly usable as a scaled index;
/// \p dTemp is clobbered either way.
/// \pre dTemp != dInput, because both are used in the comparison.
///
/// x86-64: arm64 does this with fcvtzu, a *saturating unsigned* conversion
/// that x86 has no instruction for at all. It is emulated here with a signed
/// 64-bit vcvttsd2si followed by a truncation of the result to its low 32
/// bits, which are then converted back as an unsigned value (the zero-extended
/// 32-bit mov leaves a 64-bit value in [0, 2^32), and vcvtsi2sd of that is
/// exact). The two differ instruction for instruction, but they accept and
/// reject exactly the same inputs, because in every case where fcvtzu's
/// saturation and this truncation disagree about the intermediate integer,
/// both intermediates fail the round-trip comparison:
///  - d < 0 (e.g. -1.0): fcvtzu saturates to 0, whose 0.0 differs from d;
///    here trunc gives a negative integer whose low 32 bits, read unsigned,
///    are some value in [0, 2^32) -- a non-negative double, which likewise
///    differs from the negative d. The single exception is -0.0, which both
///    forms convert to 0 and both accept, since +0.0 == -0.0. That matches
///    the interpreter, where -0 is the index 0.
///  - d >= 2^32: fcvtzu saturates to UINT32_MAX, and (double)UINT32_MAX <
///    2^32 <= d; here the low 32 bits are also some value below 2^32, so
///    again strictly less than d. Both reject.
///  - |d| >= 2^63 or d is an infinity: vcvttsd2si does not saturate, it
///    produces the "integer indefinite" value INT64_MIN, whose low 32 bits
///    are 0, and 0.0 differs from any such d. (This is why, unlike
///    emit_double_is_int() above, no INT64_MIN special case is needed: that
///    function's round trip can *legitimately* produce -2^63, this one's
///    cannot produce anything outside [0, 2^32).)
///  - d has a fractional part: both truncate it away and the round trip
///    fails on the fraction, whatever the integer part was.
///
/// The one real divergence is NaN, and it is the same one emit_double_is_int()
/// documents: arm64's fcmp reports unordered as NE, so a single b.eq covers
/// it, while vucomisd reports unordered as ZF=PF=CF=1, i.e. as *equal*. A NaN
/// index converts to INT64_MIN, whose low 32 bits are 0, so without the extra
/// test a NaN would be accepted as the in-bounds index 0. The caller must
/// therefore branch on both jne (ordered mismatch) and jp (unordered).
inline void emit_double_is_uint32(
    x86::Assembler &a,
    const x86::Gp &xTemp,
    const x86::Xmm &dTemp,
    const x86::Xmm &dInput) {
  assert(dTemp != dInput && "must use a different temp");
  assert(xTemp.isGpq() && "the conversion must go through 64 bits");

  // Convert the operand to a signed 64 bit integer.
  a.vcvttsd2si(xTemp, dInput);
  // Keep only the low 32 bits, zero-extended: this is the "unsigned 32-bit"
  // half of arm64's fcvtzu.
  a.mov(xTemp.r32(), xTemp.r32());
  // Convert back to a double and see if they compare equal.
  a.vcvtsi2sd(dTemp, dTemp, xTemp);
  a.vucomisd(dTemp, dInput);
}

/// Convert the low 32 bits of \p xInt to a double in \p dRes, interpreting
/// them as unsigned if \p isUnsigned and as signed otherwise.
/// \p xInt is clobbered when \p isUnsigned.
///
/// x86-64: this stands in for arm64's scvtf/ucvtf of a W register. x86 has
/// no unsigned integer-to-double conversion at all, so the unsigned case
/// zero-extends into 64 bits -- a 32-bit mov does that for free -- and
/// converts that as a signed 64-bit value, which is exact for every uint32.
inline void emit_int32_to_double(
    x86::Assembler &a,
    const x86::Xmm &dRes,
    const x86::Gp &xInt,
    bool isUnsigned) {
  assert(xInt.isGpq() && "the unsigned conversion needs the full register");
  if (isUnsigned) {
    a.mov(xInt.r32(), xInt.r32());
    a.vcvtsi2sd(dRes, dRes, xInt);
  } else {
    a.vcvtsi2sd(dRes, dRes, xInt.r32());
  }
}

/// Encode the boolean in the low bit of \p inOut as a HermesValue in place.
/// The high bits of \p inOut must be zero.
///
/// x86-64: arm64 folds the tag in with `movk`, which x86 has no equivalent
/// of, and the tag constant is too large for a sign-extended imm32, so the
/// caller supplies \p tempReg to materialize it.
inline void emit_sh_ljs_bool(
    x86::Assembler &a,
    const x86::Gp &inOut,
    const x86::Gp &tempReg) {
  static constexpr SHLegacyValue baseBool = HermesValue::encodeBoolValue(false);
  static_assert(HERMESVALUE_VERSION == 2);
  static_assert(
      (llvh::isShiftedUInt<16, kHV_NumDataBits>(baseBool.raw)),
      "Boolean tag must be 16 bits.");
  assert(tempReg != inOut && "temp register must differ from the input");
  a.shl(inOut, kHV_BoolBitIdx);
  // Add the bool tag.
  a.mov(tempReg, asmjit::Imm(baseBool.raw));
  a.or_(inOut, tempReg);
}

/// Store the HermesValue for the bool \p val into \p out.
///
/// x86-64: any 64-bit constant is a single `mov`, so unlike arm64 there is
/// no mov/movk pair for the true case.
inline void
emit_sh_ljs_bool_const(x86::Assembler &a, const x86::Gp &out, bool val) {
  static constexpr SHLegacyValue baseBool = HermesValue::encodeBoolValue(false);
  static_assert(HERMESVALUE_VERSION == 2);
  a.mov(
      out,
      asmjit::Imm(
          baseBool.raw | (val ? (uint64_t)1 << kHV_BoolBitIdx : (uint64_t)0)));
}

/// Load the lengthAndFlags of a HermesValue that contains a StringPrimitive
/// into \p out.
/// out and in may be the same, but in will be clobbered.
///
/// x86-64: inline here rather than out of line as on arm64; the body is two
/// instructions. The 32-bit load zeroes the upper half of \p out.
inline void emit_stringprim_get_length_and_flags(
    x86::Assembler &a,
    const x86::Gp &out,
    const x86::Gp &in) {
  emit_sh_ljs_get_pointer(a, out, in);
  a.mov(
      out.r32(),
      x86::dword_ptr(out, RuntimeOffsets::stringPrimitiveLengthAndFlags));
}

/// Emit code to check whether the input reg holds undefined, using the
/// specified temp register, which must differ from the input.
/// CPU flags are updated as a result: je on success.
///
/// x86-64: arm64 extracts the tag with an arithmetic shift and compares that.
/// Here the whole undefined bit pattern is materialized into the temp
/// instead, because x86 has a one-instruction 64-bit immediate load and no
/// three-operand shift. Undefined carries no payload -- it is a single bit
/// pattern -- so comparing the full value is exactly the tag check.
inline void emit_sh_ljs_is_undefined(
    x86::Assembler &a,
    const x86::Gp &tempReg,
    const x86::Gp &inputReg) {
  static_assert(
      HERMESVALUE_VERSION == 2,
      "undefined must be a single bit pattern with no payload");
  assert(tempReg != inputReg && "temp register must differ from the input");
  a.mov(tempReg, asmjit::Imm(_sh_ljs_undefined().raw));
  a.cmp(inputReg, tempReg);
}

/// Emit code to initialize the fields of a newly created JSObject.
/// \param obj contains a pointer to the object to initialise.
/// \param tempOrPropStorageOpt if \p hasPropStorage, contains an uncompressed
///   pointer to the property storage, which is used to initialize the
///   PropStorage, otherwise it's used as a temporary register.
///   Either way, the value in tempOrPropStorageOpt WILL be overwritten.
/// \param clazzOpt if invalid will use the default JSObject HiddenClass,
///   otherwise a compressed pointer to the HiddenClass of the new object.
void emit_jsobject_init(
    x86::Assembler &a,
    const x86::Gp &obj,
    const x86::Gp &tempOrPropStorageOpt,
    bool hasPropStorage,
    const x86::Gp &clazzOpt = x86::Gp{});

/// Emit code to initialize the fields of a newly created environment.
/// \param newEnvPtr contains a pointer to the object to initialise.
/// \param parentEnv contains a compressed pointer to the parent environment.
/// \param temp is a temporary register for use by the emitted code.
/// \param vTemp is a temporary vector register for use by the emitted code.
/// \param size is the number of slots in the new environment.
void emit_environment_init(
    x86::Assembler &a,
    const x86::Gp &newEnvPtr,
    const x86::Gp &parentEnv,
    const x86::Gp &temp,
    const x86::Xmm &vTemp,
    uint32_t size);

/// Helper function to load a pointer to the builtin closure with index
/// \p builtinIndex and place it in \p res.
void emit_load_builtin_closure(
    x86::Assembler &a,
    const x86::Gp &res,
    uint32_t builtinIndex);

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

#ifndef ASMJIT_NO_LOGGING
class OurLogger : public asmjit::Logger {
 private:
  x86::Assembler &a_;
  PerfJitDump *perfJitDump_{nullptr};
  bool dumpJitCode_{false};

 public:
  OurLogger(x86::Assembler &a, PerfJitDump *perfJitDump, bool dumpJitCode)
      : a_(a), perfJitDump_(perfJitDump), dumpJitCode_(dumpJitCode) {}

  ASMJIT_API asmjit::Error _log(const char *data, size_t size) noexcept
      override;
};
#endif

} // namespace hermes::vm::x86_64

#endif // HERMESVM_JIT

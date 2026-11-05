/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JIT/Config.h"
#if HERMESVM_JIT_X86_64
#include "JitEmitter-internal.h"

#include "hermes/VM/JSObject-inline.h"

#define DEBUG_TYPE "jit"

namespace hermes::vm::x86_64 {

void Emit_sh_shv_decode::emitFirstCase([[maybe_unused]] x86::Assembler &a) {
#ifdef HERMESVM_BOXED_DOUBLES
  static_assert(HERMESVALUE_VERSION == 2, "Constructing HermesValue from bits");
  static_assert(HermesValue32::kVersion == 1, "Decoding HV32 bits");
  constexpr uint64_t kHV32TagMask = (1 << HermesValue32::kNumTagBits) - 1;

  // Start by testing the tag bits for 0, if so, this is a "compressed HV64".
  static_assert((int)HermesValue32::Tag::CompressedHV64 == 0);
  a.test(inOut, asmjit::Imm(kHV32TagMask));
  // If the tag is non-zero, jump to the pointer check.
  a.jnz(ptrLab);
  // CompressedHV64, just shift the bits if needed. Note the case where
  // HermesValue32 is actually 64 bits is only used for testing, so it is not
  // worth optimising here.
  if constexpr (sizeof(SmallHermesValue) < sizeof(HermesValue))
    a.shl(inOut, 32);
#endif
}

void Emit_sh_shv_decode::emitRestCases([[maybe_unused]] x86::Assembler &a) {
#ifndef NDEBUG
  assert(!restEmitted && "Rest cases already emitted");
  restEmitted = true;
#endif
#ifdef HERMESVM_BOXED_DOUBLES
  asmjit::Label symLab = a.newLabel();
  asmjit::Label bdLab = a.newLabel();

  static_assert(HERMESVALUE_VERSION == 2, "Constructing HermesValue from bits");
  static_assert(HermesValue32::kVersion == 1, "Decoding HV32 bits");
  constexpr uint64_t kHV32TagMask = (1 << HermesValue32::kNumTagBits) - 1;

  a.bind(ptrLab);

  // See the comments below for why the exact bits are important.
  static_assert((int)HermesValue32::Tag::String == 0b01);
  static_assert((int)HermesValue32::Tag::BigInt == 0b10);
  static_assert((int)HermesValue32::Tag::Object == 0b11);
  // We know that the tag is 3 bits, and we have already checked for 0, so we
  // can check for the 3 interesting pointer tags (which are in the range 1-3,
  // i.e., less than 2^2) just by testing the most significant bit of the tag.
  //
  // x86-64: arm64's tbnz is a test-and-branch in one instruction; here it is
  // a `test` of that single bit followed by a jnz.
  a.test(inOut, asmjit::Imm(0b100));
  a.jnz(symLab);
  // We can decompress the pointer right away, since the HV32 representation
  // just makes it look like an unaligned compressed pointer.
  emit_sh_cp_decode_non_null(a, inOut);
  // Now we insert the new tag. First compute the "base" tag. The HV64 tag for
  // a pointer can be computed by adding the HV32 tag to this base.
  constexpr uint16_t baseTag =
      (uint16_t)HermesValue::Tag::Str - (uint16_t)HermesValue32::Tag::String;
  // We know that the base tag has zeroes in its low 2 bits, and the HV32 tag
  // is known to be in the low 2 bits at this point, so the two simply or
  // together.
  static_assert((baseTag & 0b11) == 0);
  //
  // x86-64: arm64 builds the tag field in place with a movk followed by a bfi
  // that copies the two HV32 tag bits up into it. x86 has neither, and the
  // finished tag constant is far too wide for a sign-extended imm32, so the
  // whole 16-bit field is computed in xScratch instead and or-ed in after the
  // low tag bits have been cleared out of the pointer. See the note on the
  // class: xScratch must be dead at every call site.
  //
  // The or-merge below is equivalent to arm64's movk, which *replaces* the
  // top 16 bits, only because bits 63:48 of the decoded pointer are already
  // zero. They are: this is a heap pointer on x86-64, where user-space
  // virtual addresses are 47-bit (or 56-bit with LA57, still leaving 63:57
  // clear and 56:48 set only in kernel space), and it was reconstructed by
  // adding the runtime base to a 32-bit compressed pointer, so it cannot
  // carry high bits either. If this decode is ever reached with a pointer
  // that has anything in 63:48, the tag has to be cleared first -- which on
  // x86 means the rol/mov-r16/ror sequence the symbol case below uses.
  a.mov(xScratch, inOut);
  a.and_(xScratch, asmjit::Imm(kHV32TagMask));
  a.or_(xScratch, asmjit::Imm((uint32_t)baseTag));
  a.shl(xScratch, kHV_NumDataBits);
  // Clear the HV32 tag in the low bits.
  a.and_(inOut, asmjit::Imm(~kHV32TagMask));
  a.or_(inOut, xScratch);
  a.jmp(doneLab);

  a.bind(symLab);
  // There are only two cases left, Symbol and BoxedDouble. We can distinguish
  // them with a single bit.
  static_assert((int)HermesValue32::Tag::BoxedDouble == 0b100);
  static_assert((int)HermesValue32::Tag::Symbol == 0b101);
  static_assert((int)HermesValue32::Tag::_Last == 0b110);
  // Test the low bit, to distinguish Symbol and BoxedDouble.
  a.test(inOut, asmjit::Imm(1));
  a.jz(bdLab);
  // SymbolID has a 17-bit tag in HV64, which means the tag cannot be inserted
  // in a single instruction. However, we can exploit the fact that HV64 tags
  // are particular double NaN's, with a sign bit of 1 and all-1 exponents, so
  // they start with 0xfff. This means that the most significant bits of the
  // tag are 1, and we can shift in those bits with an arithmetic shift right
  // below as we shift out the HV32 tag. First, insert the low 14 bits of the
  // symbol tag as a 16 bit immediate.
  constexpr uint16_t symTag =
      (uint16_t)((uint32_t)HVETag_Symbol << (HermesValue32::kNumTagBits - 1));
  // Assert that the top bits are preserved with an arithmetic right shift.
  static_assert(
      ((int16_t)symTag) >> (HermesValue32::kNumTagBits - 1) == HVETag_Symbol);
  //
  // x86-64: this is arm64's movk of the top 16 bits. x86 can only write the
  // *low* 16-bit subregister, so the value is rotated to bring the tag field
  // down, overwritten there, and rotated back. Unlike the pointer case above
  // this needs no scratch, because the field is replaced wholesale rather
  // than combined with bits of the input.
  constexpr unsigned kTagBits = 64 - kHV_NumDataBits;
  static_assert(kTagBits == 16, "the tag must be exactly the top 16 bits");
  a.rol(inOut, kTagBits);
  a.mov(inOut.r16(), asmjit::Imm(symTag));
  a.ror(inOut, kTagBits);
  // Next, shift in the high 1's as we shift out the HV32 tag.
  a.sar(inOut, HermesValue32::kNumTagBits);
  a.jmp(doneLab);

  a.bind(bdLab);
  // It is a boxed double. As with pointers, we can decode it as a compressed
  // pointer first.
  emit_sh_cp_decode_non_null(a, inOut);
  // Since the tag is still present, subtract it from the offset.
  constexpr size_t bdOffs = RuntimeOffsets::boxedDoubleValue -
      (size_t)HermesValue32::Tag::BoxedDouble;
  static_assert(
      bdOffs <= (size_t)INT32_MAX, "boxed double offset must fit a disp32");
  a.mov(inOut, x86::qword_ptr(inOut, (int32_t)bdOffs));
#endif
}

#ifdef HERMESVM_BOXED_DOUBLES
void emit_shv_encode_or_slow(
    x86::Assembler &a,
    const x86::Gp &out,
    const x86::Gp &in,
    const x86::Gp &temp,
    const asmjit::Label &slowLab) {
  assert(out != in && "the output must not alias the input");
  assert(temp != in && temp != out && "the scratch must differ from both");
  static_assert(HERMESVALUE_VERSION == 2, "Reading a HermesValue's ETag");
  static_assert(HermesValue32::kVersion == 1, "Encoding HV32 bits");

  // The comment is emitted here rather than by the callers so that the
  // default heap-value mode, where this function does not exist, emits
  // nothing whatsoever -- not even a comment line. Emitter::commentV() ends
  // in exactly this call.
  a.comment("// Encode the value as a SmallHermesValue");

  asmjit::Label ptrLab = a.newLabel();
  asmjit::Label symLab = a.newLabel();
  asmjit::Label doneLab = a.newLabel();

  // HermesValue32::encodeHermesValue(), case for case:
  //
  //   number or compressible -> a right shift, or a BoxedDouble allocation
  //                             this cannot do: decline;
  //   String/BigInt/Object   -> a compressed pointer with a remapped tag;
  //   Symbol                 -> the id shifted up over the tag.
  //
  // The dispatch is on the ETag rather than the tag, because the first case
  // is an ETag range test -- HermesValue::isNumberOrCompressible() -- and the
  // other two fall out of the same value with no second shift.
  a.mov(out, in);
  a.sar(out, kHV_NumDataBits - 1);
  // isNumberOrCompressible(): (uint32_t)etag <= LastNumberOrCompressible,
  // unsigned, which is exactly the comparison the runtime makes. Every
  // double's ETag is either a small non-negative number or below
  // HVETag_Empty; the tags this excludes -- Symbol, RawHV32 and the three
  // pointer tags -- are precisely the ones that sort ABOVE it once the
  // negative ETags are read as unsigned.
  static_assert(
      (int16_t)HVETag_LastNumberOrCompressible == (int16_t)(-10),
      "HVETag_LastNumberOrCompressible must be -10");
  a.cmp(out.r32(), asmjit::Imm((int32_t)HVETag_LastNumberOrCompressible));
  a.ja(ptrLab);

#if HERMESVM_SANITIZE_HANDLES != 0
  // Handle sanitization has encodeHermesValue() allocate on EVERY non-pointer
  // encode -- canInlineCompressibleOrNumberHV64() refuses to inline any
  // number, so that callers cannot get away with treating a
  // SmallHermesValue holding one as anything but a pointer, and
  // encodeHermesValue() additionally forces a throwaway encodeNumberValue()
  // for its side effect of moving the heap. Emitted code cannot allocate, so
  // this whole case is declined to the helper, which does both.
  //
  // Be honest about what that does NOT cover: only this arm declines. The
  // symbol arm below still encodes inline, so a JIT symbol store skips the
  // heap move that the runtime's encodeSymbolValue() path would have
  // triggered, and a stale handle that only that store would have exposed
  // stays hidden. That is a loss of sanitizer coverage, not of correctness --
  // the encoding written is the one the runtime writes -- and closing it
  // would mean declining every store under Handle-San, which is a bigger
  // behavior change than the sanitizer is worth here.
  a.jmp(slowLab);
#else
  // canInlineCompressibleOrNumberHV64(): the value is inline-representable
  // exactly when its low 64 - kNumValueBits bits are zero. Shifting those
  // bits up to the top of the register is that test -- shl sets ZF from its
  // result -- and costs no immediate wide enough to need a register.
  a.mov(temp, in);
  a.shl(temp, RuntimeOffsets::kShvValueBits);
  // A double that does not fit has to become a heap-allocated BoxedDouble.
  // Nothing has been stored at this point, so declining is free and the
  // helper does the allocation.
  a.jnz(slowLab);
  // bitsToCompressedHV64(): the CompressedHV64 tag is zero, so the encode is
  // the shift alone, and it is not even that when a SmallHermesValue is as
  // wide as a HermesValue (the boxed build without compressed pointers).
  a.mov(out, in);
  if constexpr (RuntimeOffsets::kShvRawTypeBits < 64)
    a.shr(out, 64 - RuntimeOffsets::kShvRawTypeBits);
  a.jmp(doneLab);
#endif

  a.bind(ptrLab);
  // Symbol and RawHV32 are the only ETags left below the pointers, so one
  // more unsigned compare separates the pointer cases from them.
  static_assert(
      (int16_t)HVETag_FirstPointer == (int16_t)(-6),
      "HVETag_FirstPointer must be -6");
  a.cmp(out.r32(), asmjit::Imm((int32_t)HVETag_FirstPointer));
  a.jb(symLab);
  // encodePointerImpl(): a compressed pointer with the tag or-ed into its low
  // bits, which are zero by heap alignment. The tag is the HermesValue's own,
  // biased by a constant (toHV32Tag()); recovering it from the ETag is a
  // second arithmetic shift, and the addition leaves a value in 1..3 with a
  // clear upper half, so the or below cannot disturb the pointer.
  a.sar(out, 1);
  a.add(out, asmjit::Imm(RuntimeOffsets::kShvPointerTagBias));
  emit_sh_ljs_get_pointer(a, temp, in);
  emit_sh_cp_encode_non_null(a, temp);
  a.or_(out, temp);
  a.jmp(doneLab);

  a.bind(symLab);
  // encodeHermesValue() asserts at this point that what is left is a symbol.
  // An assert is not available here, and RawHV32 -- the one other ETag that
  // reaches this branch -- has no SmallHermesValue encoding at all, so
  // anything that is not a symbol is declined instead.
  static_assert(
      (int16_t)HVETag_Symbol == (int16_t)(-9), "HVETag_Symbol must be -9");
  a.cmp(out.r32(), asmjit::Imm((int32_t)HVETag_Symbol));
  a.jne(slowLab);
  // fromTagAndValue(Tag::Symbol, id): a SymbolID is a uint32 held in the low
  // half of the HermesValue, so the 32-bit move both extracts it and clears
  // the upper half.
  a.mov(out.r32(), in.r32());
  a.shl(out, RuntimeOffsets::kShvTagBits);
  a.or_(out, asmjit::Imm((uint32_t)HermesValue32::Tag::Symbol));

  a.bind(doneLab);
}
#endif // HERMESVM_BOXED_DOUBLES

void emit_jsobject_init(
    x86::Assembler &a,
    const x86::Gp &obj,
    const x86::Gp &tempOrPropStorageOpt,
    bool hasPropStorage,
    const x86::Gp &clazzOpt) {
  // obj->flags = 0
  // x86-64: a 32-bit store of an immediate needs no register, so arm64's
  // wzr has no counterpart to give up here.
  a.mov(x86::dword_ptr(obj, offsetof(SHJSObject, flags)), asmjit::Imm(0));

  if (hasPropStorage) {
    emit_sh_cp_encode_non_null(a, tempOrPropStorageOpt);
    emit_store_cp(
        a,
        tempOrPropStorageOpt,
        x86::ptr(obj, offsetof(SHJSObject, propStorage)));
  }

  // No longer need the propStorage pointer. Alias for clarity.
  const x86::Gp &temp = tempOrPropStorageOpt;

  if (!clazzOpt.isValid()) {
    // Load the hidden class compressed pointer into clazz.
    a.mov(temp, x86::qword_ptr(xRuntime, offsetof(Runtime, classJSObject)));
    emit_sh_ljs_get_pointer(a, temp, temp);
    emit_sh_cp_encode_non_null(a, temp);
  }

  const x86::Gp &clazz = clazzOpt.isValid() ? clazzOpt : temp;

  // Store the hidden class, which also determines the parent.
  // obj->clazz = clazz (may be the same register as temp).
  assert(clazz.isValid());
  emit_store_cp(a, clazz, x86::ptr(obj, offsetof(SHJSObject, clazz)));

  // If !hasPropStorage, obj->propStorage = nullptr.

  // obj->directProps[N] = SmallHermesValue::encodeRawZeroValue()

  // We want to zero the rest of the object. To simplify things, we align the
  // size to the heap alignment of 8 bytes, which ensures that the end of the
  // fill region is aligned to 8 bytes.
  // Start the fill at propStorage if HasPropStorage is false,
  // otherwise start after propStorage in the directProps.
  size_t startZeroOffset = hasPropStorage
      ? offsetof(SHJSObjectAndDirectProps, directProps)
      : offsetof(SHJSObject, propStorage);
  size_t bytesToZero = heapAlignSize(cellSize<JSObject>()) - startZeroOffset;
  size_t zeroedBytes = 0;
  assert(bytesToZero % 4 == 0 && "Must be a multiple of 4");

  // x86-64: `mov mem, imm` needs no source register, so unlike arm64 there is
  // nothing to gain from pairing the stores and the 16-byte step disappears.
  // If there is some amount that is not a multiple of 8, store that first.
  if (bytesToZero & 4) {
    a.mov(x86::dword_ptr(obj, (int32_t)startZeroOffset), asmjit::Imm(0));
    zeroedBytes += 4;
  }

  // The rest of the fill region is a multiple of 8 and, since the end of the
  // region is 8-byte aligned, so is every store below.
  for (; zeroedBytes < bytesToZero; zeroedBytes += 8) {
    a.mov(
        x86::qword_ptr(obj, (int32_t)(startZeroOffset + zeroedBytes)),
        asmjit::Imm(0));
  }
  assert(zeroedBytes == bytesToZero && "Did not zero the whole object");
}

/// Emit code to initialize the fields of a newly created environment.
/// \param newEnvPtr contains a pointer to the object to initialise.
/// \param parentEnv contains a compressed pointer to the parent environment.
/// \param temp is a temporary register for use by the emitted code.
/// \param vTemp is a temporary vector register for use by the emitted code.
/// \param size is the number of slots in the new environment.
///
/// No write barrier is emitted for the parent pointer, and none is needed:
/// the cell was just bump-allocated in the young generation, so it cannot be
/// an old-gen object pointing at a young one, and it is not yet reachable
/// from anything the GC scans. The same reasoning covers the slots, which are
/// filled with undefined. This mirrors arm64 exactly.
void emit_environment_init(
    x86::Assembler &a,
    const x86::Gp &newEnvPtr,
    const x86::Gp &parentEnv,
    const x86::Gp &temp,
    const x86::Xmm &vTemp,
    uint32_t size) {
  // Store the size and parent to the first two fields.
  // x86-64: arm64 pairs the two narrow stores with stp, which forces the size
  // into a register first. Here a store of an immediate is a single
  // instruction in both compressed-pointer configurations, so the two cases
  // collapse into one and differ only in the width of the parent store, which
  // emit_store_cp() picks.
  emit_store_cp(
      a,
      parentEnv,
      x86::ptr(newEnvPtr, offsetof(SHEnvironment, parentEnvironment)));
  a.mov(
      x86::dword_ptr(newEnvPtr, offsetof(SHEnvironment, size)),
      asmjit::Imm(size));

  // Load undefined.
  a.mov(temp, asmjit::Imm(_sh_ljs_undefined().raw));

  // x86-64: every slot is reachable through the store's signed 32-bit
  // displacement, so arm64's post-indexed walk over a slot pointer (and the
  // register it consumes) is replaced by folded displacements. The vector
  // stores are unaligned: a cell is only guaranteed 8-byte aligned, so the
  // 16-byte stores below must not require more.
  auto slotsToFill = size;
  /// \return the displacement of the first unfilled slot, plus \p extra.
  const auto slotOfs = [size](uint32_t remaining, uint32_t extra) {
    size_t ofs = offsetof(SHEnvironment, slots) +
        sizeof(SHLegacyValue) * (size_t)(size - remaining) + extra;
    assert(ofs <= (size_t)INT32_MAX && "slot offset must fit a disp32");
    return (int32_t)ofs;
  };

  // Before we fill 4 at a time, make sure any excess slots are filled.
  if (slotsToFill % 2) {
    a.mov(x86::qword_ptr(newEnvPtr, offsetof(SHEnvironment, slots)), temp);
    --slotsToFill;
  }
  if (slotsToFill % 4) {
    // x86-64: arm64's stp of the same register twice is a pair of stores.
    a.mov(x86::qword_ptr(newEnvPtr, slotOfs(slotsToFill, 0)), temp);
    a.mov(x86::qword_ptr(newEnvPtr, slotOfs(slotsToFill, 8)), temp);
    slotsToFill -= 2;
  }

  // The remaining number of slots must be a multiple of 4, which we can fill 4
  // at a time.
  assert((slotsToFill % 4) == 0 && "Remaining slots must be multiple of 4");
  if (slotsToFill) {
    // Copy the value into a vector register, so we can fill faster. vmovq
    // zeroes the upper half, which vpunpcklqdq then overwrites with a second
    // copy of the low half -- arm64 does both in one `dup`.
    a.vmovq(vTemp, temp);
    a.vpunpcklqdq(vTemp, vTemp, vTemp);

    // Fill the slots with undefined 4 at a time. Each 16-byte store covers
    // two slots, where arm64's stp of two vector registers covers four.
    for (; slotsToFill; slotsToFill -= 4) {
      a.vmovups(x86::xmmword_ptr(newEnvPtr, slotOfs(slotsToFill, 0)), vTemp);
      a.vmovups(x86::xmmword_ptr(newEnvPtr, slotOfs(slotsToFill, 16)), vTemp);
    }
  }
  assert(slotsToFill == 0 && "All slots must be filled");
}

/// Helper function to load a pointer to the builtin closure with index
/// \p builtinIndex and place it in \p res.
void emit_load_builtin_closure(
    x86::Assembler &a,
    const x86::Gp &res,
    uint32_t builtinIndex) {
  static_assert(
      std::is_same_v<
          TransparentOwningPtr<Callable *, llvh::FreeDeleter>,
          RuntimeOffsets::BuiltinsType>,
      "builtins_ is a list of Callable *");
  static_assert(
      offsetof(TransparentOwningPtr<Callable *>, ptr) == 0,
      "TransparentOwningPtr must be transparent");

  a.mov(res, x86::qword_ptr(xRuntime, RuntimeOffsets::builtins));
  a.mov(res, x86::qword_ptr(res, (int32_t)(builtinIndex * sizeof(Callable *))));
}

void OurErrorHandler::handleError(
    asmjit::Error err,
    const char *message,
    asmjit::BaseEmitter *origin) {
  if (err == expectedError_) {
    LLVM_DEBUG(
        llvh::outs() << "Expected AsmJit error: " << err << ": "
                     << asmjit::DebugUtils::errorAsString(err) << ": "
                     << message << "\n");
    return;
  }

  std::string formattedMsg{};
  {
    // Ensure we run any destructors for the ostream before longjmp.
    llvh::raw_string_ostream OS{formattedMsg};
    OS << "AsmJit error: " << err << ": "
       << asmjit::DebugUtils::errorAsString(err) << ": " << message;
    OS.flush();
  }

  // IMPORTANT: From here on, we MUST ensure that no destructors need to run.
  // One exception: formattedMsg will have its destructor skipped, but we're
  // moving out of it so in practice the std::string won't have anything to
  // free, avoiding leaks.
  LLVM_DEBUG(llvh::dbgs() << formattedMsg << "\n");
  longjmpError_(std::move(formattedMsg));
}

#ifndef ASMJIT_NO_LOGGING
asmjit::Error OurLogger::_log(const char *data, size_t size) noexcept {
  auto str =
      (size == SIZE_MAX ? llvh::StringRef(data) : llvh::StringRef(data, size));
  if (str.empty())
    return asmjit::kErrorOk;
  if (dumpJitCode_)
    llvh::outs() << str;
  if (!perfJitDump_)
    return asmjit::kErrorOk;

  // Comments by default do not have indentation, except some pseudocode
  // comments, which start with ';' after indentation.
  auto trimmed = str.ltrim();
  if (str.front() != ' ' || (!trimmed.empty() && trimmed.front() == ';')) {
    perfJitDump_->addCodeComment(str, a_.offset());
  }
  return asmjit::kErrorOk;
}
#endif

#if HERMES_JIT_INLINE_SAFE_STORE
void Emitter::emitSafeStoreOrSlow(
    const x86::Gp &loc,
    const x86::Gp &shv,
    const x86::Gp &value,
    const x86::Gp &t1,
    const x86::Gp &t2,
    const asmjit::Label &slowLab) {
  assert(t1 != loc && t1 != value && "t1 must not alias loc or value");
  assert(t2 != loc && t2 != value && "t2 must not alias loc or value");
  assert(t1 != t2 && "the two temporaries must differ");
  assert(
      shv != loc && shv != t1 && "the encoded value must survive to the store");
  // \p shv MAY be \p t2, and under boxed doubles it is: the caller encoded
  // into t2 before its guards. That is safe because t2 is not touched here
  // until after the store, by which point the encoded value is dead.

  comment("// Inline store with barrier predicate");

  asmjit::Label youngTargetLab = a.newLabel();
  asmjit::Label doneLab = a.newLabel();

  // The mask that turns a pointer into the start of its segment. It is
  // negative, so it reaches the emitted instruction as a sign-extended imm32.
  static_assert(
      RuntimeOffsets::kSegmentUnitSize <= ((size_t)1 << 31),
      "the segment mask must fit a sign-extended imm32");
  constexpr int64_t kSegmentMask =
      ~(int64_t)(RuntimeOffsets::kSegmentUnitSize - 1);

  // t1 = the start of the segment containing the slot.
  a.mov(t1, loc);
  a.and_(t1, asmjit::Imm(kSegmentMask));

  // HadesGC::writeBarrier() step (1): a slot in the young generation never
  // needs a barrier of any kind. RuntimeOffsets::runtimeHadesYGStart is
  // loaded rather than baked in because setYoungGen() can swap the segment.
  a.cmp(t1, x86::qword_ptr(xRuntime, RuntimeOffsets::runtimeHadesYGStart));
  a.je(youngTargetLab);

  // Step (2): if the OG marking barriers are on, the snapshot barrier must
  // read the slot's OLD value, so the store cannot happen here. Testing this
  // before storing anything is what makes that ordering hold by
  // construction. RuntimeOffsets::runtimeHadesOGMarkingBarriers.
  a.cmp(
      x86::byte_ptr(xRuntime, RuntimeOffsets::runtimeHadesOGMarkingBarriers),
      asmjit::Imm(0));
  a.jne(slowLab);

  // Step (3), first half: relocationWriteBarrier() also dirties a card for a
  // newly created pointer into the segment being compacted. Rather than
  // replicate that, decline whenever a compaction is in progress at all.
  // RuntimeOffsets::runtimeHadesCompacteeStart / kHadesNoCompactee.
  static_assert(
      RuntimeOffsets::kHadesNoCompactee <= (uintptr_t)INT32_MAX,
      "the no-compactee sentinel must fit an imm32");
  a.cmp(
      x86::qword_ptr(xRuntime, RuntimeOffsets::runtimeHadesCompacteeStart),
      asmjit::Imm((int32_t)RuntimeOffsets::kHadesNoCompactee));
  a.jne(slowLab);

  // The card-dirty store at the bottom writes the segment's INLINE card
  // status array, which only exists in a segment exactly one unit long; a
  // jumbo segment keeps its card array out of line and is barriered through
  // AlignedHeapSegment::dirtyCardForAddressInLargeObj(). Declining here is
  // what makes the card math valid.
  //
  // It is NOT, on its own, what makes this safe for an arbitrary address:
  // this load only reads SHSegmentInfo when `loc` is inside the first unit
  // of its segment, which is the precondition documented on the declaration.
  // A jumbo segment is unit-ALIGNED but several units long, so for a `loc`
  // further in, `loc & ~(unit-1)` names a later unit and the word below is
  // object payload. Callers bound `loc`: PutById through
  // WritePropertyCacheEntry::kMaxSlot, array stores through a storage-size
  // gate. RuntimeOffsets::segmentShiftedSize.
  a.cmp(x86::word_ptr(t1, RuntimeOffsets::segmentShiftedSize), asmjit::Imm(1));
  a.jne(slowLab);

  // The barrier is now known to be at most a card-dirty. Store.
  emit_store_shv(a, shv, x86::ptr(loc));

  // relocationWriteBarrier() is only reached for a pointer value. The test is
  // made on the ORIGINAL HermesValue rather than on what was just stored,
  // which is both cheaper and more accurate: a HermesValue carries its
  // pointer uncompressed, which is what the segment compare below needs, and
  // the one pointer a SmallHermesValue has that a HermesValue does not -- a
  // BoxedDouble -- can never be here, because the encode declined it. \p t2
  // is dead once the store above has consumed it.
  emit_sh_ljs_get_tag(a, t2, value);
  emit_sh_ljs_tag_is_pointer(a, t2);
  a.jb(doneLab);

  // t2 = the start of the segment containing the pointed-to cell. Only an
  // old-to-young pointer needs a card; old-to-old does not, and the compactee
  // cases were declined above.
  emit_sh_ljs_get_pointer(a, t2, value);
  a.and_(t2, asmjit::Imm(kSegmentMask));
  a.cmp(t2, x86::qword_ptr(xRuntime, RuntimeOffsets::runtimeHadesYGStart));
  a.jne(doneLab);

  // FixedSizeHeapSegment::dirtyCardForAddress(loc), inline:
  //   segLoc[(loc - segLoc) >> kLogCardSize] = CardStatus::Dirty
  // The card array base is the segment start itself, since
  // RuntimeOffsets::segmentInlineCards is 0. The runtime writes this byte
  // with a relaxed atomic store, which on x86 is this same instruction.
  a.mov(t2, loc);
  a.sub(t2, t1);
  a.shr(t2, asmjit::Imm(RuntimeOffsets::kLogCardSize));
  a.mov(
      x86::byte_ptr(t1, t2, 0, (int32_t)RuntimeOffsets::segmentInlineCards),
      asmjit::Imm(RuntimeOffsets::kCardDirty));
  a.jmp(doneLab);

  a.bind(youngTargetLab);
  emit_store_shv(a, shv, x86::ptr(loc));

  a.bind(doneLab);
}
#endif // HERMES_JIT_INLINE_SAFE_STORE

} // namespace hermes::vm::x86_64
#endif // HERMESVM_JIT_X86_64

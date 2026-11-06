/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JIT/Config.h"
#if HERMESVM_JIT_ARM64

#include "JitEmitter-internal.h"

#include "hermes/VM/JSObject-inline.h"
#include "llvh/Support/Debug.h"

#define DEBUG_TYPE "jit"

namespace hermes::vm::arm64 {

void Emit_sh_shv_decode::emitFirstCase(a64::Assembler &a) {
#ifdef HERMESVM_BOXED_DOUBLES
  static_assert(HERMESVALUE_VERSION == 2, "Constructing HermesValue from bits");
  static_assert(HermesValue32::kVersion == 1, "Decoding HV32 bits");
  constexpr uint64_t kHV32TagMask = (1 << HermesValue32::kNumTagBits) - 1;

  // Start by testing the tag bits for 0, if so, this is a "compressed HV64".
  static_assert((int)HermesValue32::Tag::CompressedHV64 == 0);
  a.tst(xInOut, kHV32TagMask);
  // If the tag is non-zero, jump to the pointer check.
  a.b_ne(ptrLab);
  // CompressedHV64, just shift the bits if needed. Note the case where
  // HermesValue32 is actually 64 bits is only used for testing, so it is not
  // worth optimising here.
  if constexpr (sizeof(SmallHermesValue) < sizeof(HermesValue))
    a.lsl(xInOut, xInOut, 32);
#endif
}

void Emit_sh_shv_decode::emitRestCases(a64::Assembler &a) {
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
  a.tbnz(xInOut, 2, symLab);
  // We can decompress the pointer right away, since the HV32 representation
  // just makes it look like an unaligned compressed pointer.
  emit_sh_cp_decode_non_null(a, xInOut);
  // Now we insert the new tag. First compute the "base" tag. The HV64 tag for a
  // pointer can be computed by adding the HV32 tag to this base.
  constexpr uint16_t baseTag =
      (uint16_t)HermesValue::Tag::Str - (uint16_t)HermesValue32::Tag::String;
  // Move in the base tag.
  a.movk(xInOut, baseTag, kHV_NumDataBits);

  // We know that the base tag has zeroes in its low 2 bits, and the HV32 tag is
  // known to be in the low 2 bits at this point. So we can use bfi to simply
  // copy those low bits over.
  static_assert((baseTag & 0b11) == 0);
  a.bfi(xInOut, xInOut, kHV_NumDataBits, 2);

  // Clear the HV32 tag in the low bits.
  a.and_(xInOut, xInOut, ~kHV32TagMask);
  a.b(doneLab);

  a.bind(symLab);
  // There are only two cases left, Symbol and BoxedDouble. We can distinguish
  // them with a single bit.
  static_assert((int)HermesValue32::Tag::BoxedDouble == 0b100);
  static_assert((int)HermesValue32::Tag::Symbol == 0b101);
  static_assert((int)HermesValue32::Tag::_Last == 0b110);
  // Test the low bit, to distinguish Symbol and BoxedDouble.
  a.tbz(xInOut, 0, bdLab);
  // SymbolID has a 17-bit tag in HV64, which means the tag cannot be inserted
  // in a single instruction. However, we can exploit the fact that HV64 tags
  // are particular double NaN's, with a sign bit of 1 and all-1 exponents, so
  // they start with 0xfff. This means that the most significant bits of the tag
  // are 1, and we can shift in those bits with an asr below as we shift out the
  // HV32 tag. First, insert the low 14 bits of the symbol tag as a 16 bit
  // immediate.
  constexpr uint16_t symTag =
      (uint16_t)((uint32_t)HVETag_Symbol << (HermesValue32::kNumTagBits - 1));
  // Assert that the top bits are preserved with an arithmetic right shift.
  static_assert(
      ((int16_t)symTag) >> (HermesValue32::kNumTagBits - 1) == HVETag_Symbol);
  a.movk(xInOut, symTag, kHV_NumDataBits);
  // Next, shift in the high 1's as we shift out the HV32 tag.
  a.asr(xInOut, xInOut, HermesValue32::kNumTagBits);
  a.b(doneLab);

  a.bind(bdLab);
  // It is a boxed double. As with pointers, we can decode it as a compressed
  // pointer first.
  emit_sh_cp_decode_non_null(a, xInOut);
  // Since the tag is still present, subtract it from the offset.
  constexpr size_t bdOffs = RuntimeOffsets::boxedDoubleValue -
      (size_t)HermesValue32::Tag::BoxedDouble;
  a.ldr(xInOut, a64::Mem(xInOut, bdOffs));
#endif
}

#ifdef HERMESVM_BOXED_DOUBLES
void emit_shv_encode_or_slow(
    a64::Assembler &a,
    const a64::GpX &xOut,
    const a64::GpX &xIn,
    const a64::GpX &xTemp,
    const asmjit::Label &slowLab) {
  assert(xOut != xIn && "the output must not alias the input");
  assert(xTemp != xIn && xTemp != xOut && "the scratch must differ from both");
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
  a.asr(xOut, xIn, kHV_NumDataBits - 1);
  // isNumberOrCompressible(): etag <= LastNumberOrCompressible read as
  // unsigned, which is exactly the comparison the runtime makes. Every
  // double's ETag is either a small non-negative number or below
  // HVETag_Empty; the tags this excludes -- Symbol, RawHV32 and the three
  // pointer tags -- are precisely the ones that sort ABOVE it once the
  // negative ETags are read as unsigned.
  //
  // arm64: the ETags are negative, and negating one makes it a small positive
  // that encodes as an ADD/SUB immediate, so this is `cmn` against its
  // negation rather than x86-64's `cmp` against a sign-extended imm32. The
  // flags are the same ones `cmp` would set -- adding N is subtracting -N --
  // so b.hi is `ja`. It is the idiom every emit_sh_ljs_is_*() next door uses.
  static_assert(
      (int16_t)HVETag_LastNumberOrCompressible == (int16_t)(-10),
      "HVETag_LastNumberOrCompressible must be -10");
  a.cmn(xOut, -HVETag_LastNumberOrCompressible);
  a.b_hi(ptrLab);

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
  a.b(slowLab);
#else
  // canInlineCompressibleOrNumberHV64(): the value is inline-representable
  // exactly when its low 64 - kShvValueBits bits are zero.
  //
  // arm64: those bits are a run of low ones, which is the shape of an AArch64
  // logical immediate, so this is a single `tst` against the mask and needs
  // neither the scratch nor the copy x86-64's shift-them-off-the-top spends.
  static_assert(
      RuntimeOffsets::kShvValueBits != 0 && RuntimeOffsets::kShvValueBits < 64,
      "the mask must be a non-empty, non-full run of ones to encode as a "
      "logical immediate");
  constexpr uint64_t kInlineMask =
      ((uint64_t)1 << (64 - RuntimeOffsets::kShvValueBits)) - 1;
  assert(
      a64::Utils::isLogicalImm(kInlineMask, 64) &&
      "the inline-representability mask must encode as a logical immediate");
  a.tst(xIn, kInlineMask);
  // A double that does not fit has to become a heap-allocated BoxedDouble.
  // Nothing has been stored at this point, so declining is free and the
  // helper does the allocation.
  a.b_ne(slowLab);
  // bitsToCompressedHV64(): the CompressedHV64 tag is zero, so the encode is
  // the shift alone, and it is not even that when a SmallHermesValue is as
  // wide as a HermesValue (the boxed build without compressed pointers).
  if constexpr (RuntimeOffsets::kShvRawTypeBits < 64)
    a.lsr(xOut, xIn, 64 - RuntimeOffsets::kShvRawTypeBits);
  else
    a.mov(xOut, xIn);
  a.b(doneLab);
#endif

  a.bind(ptrLab);
  // Symbol and RawHV32 are the only ETags left below the pointers, so one
  // more unsigned compare separates the pointer cases from them. Same `cmn`
  // form as above; b.lo is `jb`.
  static_assert(
      (int16_t)HVETag_FirstPointer == (int16_t)(-6),
      "HVETag_FirstPointer must be -6");
  a.cmn(xOut, -HVETag_FirstPointer);
  a.b_lo(symLab);
  // encodePointerImpl(): a compressed pointer with the tag or-ed into its low
  // bits, which are zero by heap alignment. The tag is the HermesValue's own,
  // biased by a constant (toHV32Tag()); recovering it from the ETag is a
  // second arithmetic shift, and the addition leaves a value in 1..3 with a
  // clear upper half, so the or below cannot disturb the pointer.
  static_assert(
      RuntimeOffsets::kShvPointerTagBias > 0,
      "a negative bias would need a SUB here, not an ADD");
  assert(
      a64::Utils::isAddSubImm(RuntimeOffsets::kShvPointerTagBias) &&
      "the pointer tag bias must encode as an ADD immediate");
  a.asr(xOut, xOut, 1);
  a.add(xOut, xOut, RuntimeOffsets::kShvPointerTagBias);
  emit_sh_ljs_get_pointer(a, xTemp, xIn);
  emit_sh_cp_encode_non_null(a, xTemp);
  a.orr(xOut, xOut, xTemp);
  a.b(doneLab);

  a.bind(symLab);
  // encodeHermesValue() asserts at this point that what is left is a symbol.
  // An assert is not available here, and RawHV32 -- the one other ETag that
  // reaches this branch -- has no SmallHermesValue encoding at all, so
  // anything that is not a symbol is declined instead.
  static_assert(
      (int16_t)HVETag_Symbol == (int16_t)(-9), "HVETag_Symbol must be -9");
  a.cmn(xOut, -HVETag_Symbol);
  a.b_ne(slowLab);
  // fromTagAndValue(Tag::Symbol, id): a SymbolID is a uint32 held in the low
  // half of the HermesValue, so the 32-bit move both extracts it and clears
  // the upper half.
  //
  // arm64: the tag goes in with an ADD rather than an ORR. The shift has just
  // cleared the low kShvTagBits bits, so over a tag that fits in them the two
  // are the same operation -- and the tag is 0b101, which is not a run of
  // ones at any element size and therefore not an AArch64 logical immediate,
  // while every value that small is an ADD immediate.
  static_assert(
      (uint32_t)HermesValue32::Tag::Symbol <
          ((uint32_t)1 << RuntimeOffsets::kShvTagBits),
      "the symbol tag must fit the bits the shift cleared, or the ADD below "
      "is not an OR");
  a.mov(xOut.w(), xIn.w());
  a.lsl(xOut, xOut, RuntimeOffsets::kShvTagBits);
  a.add(xOut, xOut, (uint32_t)HermesValue32::Tag::Symbol);

  a.bind(doneLab);
}
#endif // HERMESVM_BOXED_DOUBLES

void emit_stringprim_get_length_and_flags(
    a64::Assembler &a,
    const a64::GpX &xOut,
    const a64::GpX &xIn) {
  emit_sh_ljs_get_pointer(a, xOut, xIn);
  a.ldr(
      xOut.w(), a64::Mem(xOut, RuntimeOffsets::stringPrimitiveLengthAndFlags));
}

void emit_jsobject_init(
    a64::Assembler &a,
    const a64::GpX &xObj,
    const a64::GpX &xTempOrPropStorageOpt,
    bool hasPropStorage,
    const a64::GpX &xClazzOpt) {
  // obj->flags = 0
  a.str(a64::wzr, a64::Mem(xObj, offsetof(SHJSObject, flags)));

  if (hasPropStorage) {
    emit_sh_cp_encode_non_null(a, xTempOrPropStorageOpt);
    emit_store_cp(
        a,
        xTempOrPropStorageOpt,
        a64::Mem(xObj, offsetof(SHJSObject, propStorage)));
  }

  // No longer need the propStorage pointer. Alias for clarity.
  const a64::GpX &xTemp = xTempOrPropStorageOpt;

  if (!xClazzOpt.isValid()) {
    a.ldr(xTemp, a64::Mem(xRuntime, offsetof(Runtime, classJSObject)));
    emit_sh_ljs_get_pointer(a, xTemp, xTemp);
    emit_sh_cp_encode_non_null(a, xTemp);
  }

  const a64::GpX &xClazz = xClazzOpt.isValid() ? xClazzOpt : xTemp;

  // Store the parent and hidden class.
  // obj->parent = xParent
  // obj->clazz = xClazz (may be the same as xTemp).
  assert(xClazz.isValid());
  emit_store_cp(a, xClazz, a64::Mem(xObj, offsetof(SHJSObject, clazz)));

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

  // If there is some amount that is not a multiple of 8, store that first.
  if (bytesToZero & 4) {
    a.str(a64::wzr, a64::Mem(xObj, startZeroOffset));
    zeroedBytes += 4;
  }

  // Now store any amount that is not a multiple of 16. Note that since the end
  // of the fill region is aligned to 8 bytes, we know all further stores will
  // be aligned to 8 bytes.
  if (bytesToZero & 8) {
    a.str(a64::xzr, a64::Mem(xObj, startZeroOffset + zeroedBytes));
    zeroedBytes += 8;
  }
  // Store the rest as multiples of 16.
  for (; zeroedBytes < bytesToZero; zeroedBytes += 16) {
    a.stp(a64::xzr, a64::xzr, a64::Mem(xObj, startZeroOffset + zeroedBytes));
  }
  assert(zeroedBytes == bytesToZero && "Did not zero the whole object");
}

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
    uint32_t size) {
  // Store the size and parent to the first two fields.
  a.mov(xTemp, size);
  if constexpr (sizeof(CompressedPointer) == 4) {
    a.stp(
        xParentEnv.w(),
        xTemp.w(),
        a64::Mem(xNewEnvPtr, offsetof(SHEnvironment, parentEnvironment)));
  } else {
    a.str(
        xParentEnv,
        a64::Mem(xNewEnvPtr, offsetof(SHEnvironment, parentEnvironment)));
    a.str(xTemp.w(), a64::Mem(xNewEnvPtr, offsetof(SHEnvironment, size)));
  }

  // Load undefined.
  a.mov(xTemp, _sh_ljs_undefined().raw);

  auto slotsToFill = size;
  // Before we fill 4 at a time, make sure any excess slots are filled.
  if (slotsToFill % 2) {
    a.str(xTemp, a64::Mem(xNewEnvPtr, offsetof(SHEnvironment, slots)));
    --slotsToFill;
  }
  if (slotsToFill % 4) {
    auto slotOffs = sizeof(SHLegacyValue) * (size - slotsToFill);
    a.stp(
        xTemp,
        xTemp,
        a64::Mem(xNewEnvPtr, offsetof(SHEnvironment, slots) + slotOffs));
    slotsToFill -= 2;
  }

  // The remaining number of slots must be a multiple of 4, which we can fill 4
  // at a time.
  assert((slotsToFill % 4) == 0 && "Remaining slots must be multiple of 4");
  if (slotsToFill) {
    // Copy the value into a vector register, so we can fill faster.
    a.dup(vTemp.d2(), xTemp);

    auto slotOffs = sizeof(SHLegacyValue) * (size - slotsToFill);
    // Initialize the pointer to the slots.
    a.add(xTemp, xNewEnvPtr, offsetof(SHEnvironment, slots) + slotOffs);

    // Fill the slots with undefined 4 at a time.
    for (; slotsToFill; slotsToFill -= 4)
      a.stp(vTemp, vTemp, a64::Mem(xTemp).post(32));
  }
  assert(slotsToFill == 0 && "All slots must be filled");
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

/// Helper function to load a pointer to the builtin closure with index
/// \p builtinIndex and place it in \p xRes.
void emit_load_builtin_closure(
    a64::Assembler &a,
    const a64::GpX &xRes,
    uint32_t builtinIndex) {
  static_assert(
      std::is_same_v<
          TransparentOwningPtr<Callable *, llvh::FreeDeleter>,
          RuntimeOffsets::BuiltinsType>,
      "builtins_ is a list of Callable *");
  static_assert(
      offsetof(TransparentOwningPtr<Callable *>, ptr) == 0,
      "TransparentOwningPtr must be transparent");

  a.ldr(xRes, a64::Mem(xRuntime, RuntimeOffsets::builtins));
  a.ldr(xRes, a64::Mem(xRes, builtinIndex * sizeof(Callable *)));
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
    const a64::GpX &loc,
    const a64::GpX &shv,
    const a64::GpX &value,
    const a64::GpX &t1,
    const a64::GpX &t2,
    const asmjit::Label &slowLab) {
  assert(t1 != loc && t1 != value && "t1 must not alias loc or value");
  assert(t2 != loc && t2 != value && "t2 must not alias loc or value");
  assert(t1 != t2 && "the two temporaries must differ");
  assert(
      shv != loc && shv != t1 && "the encoded value must survive to the store");
  // \p shv MAY be \p t2, and under boxed doubles it is: the caller encoded
  // into t2 before its guards. Keeping that sound is why every runtime word
  // tested below is loaded into xScratch rather than into t2 -- x86-64 can
  // compare against memory and so needs no register at all for those, while
  // arm64 must load first, and t2 is not free to be that register.

  // The two 64-bit runtime words below are loaded with LDR's unsigned-offset
  // form, whose immediate is a 12-bit multiple of the access width. Both
  // offsets are well inside that today (2136 and 5416), but they are
  // offsetof()s into Runtime, which grows: pin them so that outgrowing the
  // encoding breaks the build rather than making every JIT compilation fail
  // at emission time.
  static_assert(
      RuntimeOffsets::runtimeHadesYGStart % 8 == 0 &&
          RuntimeOffsets::runtimeHadesYGStart < kMaxInlineBaseOffset,
      "the young-gen start must be reachable by an unsigned-offset LDR");
  static_assert(
      RuntimeOffsets::runtimeHadesCompacteeStart % 8 == 0 &&
          RuntimeOffsets::runtimeHadesCompacteeStart < kMaxInlineBaseOffset,
      "the compactee start must be reachable by an unsigned-offset LDR");

  comment("// Inline store with barrier predicate");

  asmjit::Label youngTargetLab = a.newLabel();
  asmjit::Label doneLab = a.newLabel();

  // The mask that turns a pointer into the start of its segment. A power of
  // two minus one inverted is a run of ones, which is exactly the shape of an
  // AArch64 logical immediate, so the AND below needs no scratch register.
  constexpr uint64_t kSegmentMask =
      ~(uint64_t)(RuntimeOffsets::kSegmentUnitSize - 1);
  assert(
      a64::Utils::isLogicalImm(kSegmentMask, 64) &&
      "the segment mask must encode as a logical immediate");

  // t1 = the start of the segment containing the slot.
  a.and_(t1, loc, kSegmentMask);

  // HadesGC::writeBarrier() step (1): a slot in the young generation never
  // needs a barrier of any kind. RuntimeOffsets::runtimeHadesYGStart is
  // loaded rather than baked in because setYoungGen() can swap the segment.
  a.ldr(xScratch, a64::Mem(xRuntime, RuntimeOffsets::runtimeHadesYGStart));
  a.cmp(t1, xScratch);
  a.b_eq(youngTargetLab);

  // Step (2): if the OG marking barriers are on, the snapshot barrier must
  // read the slot's OLD value, so the store cannot happen here. Testing this
  // before storing anything is what makes that ordering hold by
  // construction. RuntimeOffsets::runtimeHadesOGMarkingBarriers.
  emit_load_from_base_offset<1, true>(
      a, xScratch, xRuntime, {}, RuntimeOffsets::runtimeHadesOGMarkingBarriers);
  a.cbnz(xScratch.w(), slowLab);

  // Step (3), first half: relocationWriteBarrier() also dirties a card for a
  // newly created pointer into the segment being compacted. Rather than
  // replicate that, decline whenever a compaction is in progress at all.
  // RuntimeOffsets::runtimeHadesCompacteeStart / kHadesNoCompactee.
  assert(
      a64::Utils::isAddSubImm(RuntimeOffsets::kHadesNoCompactee) &&
      "the no-compactee sentinel must encode as a compare immediate");
  a.ldr(
      xScratch, a64::Mem(xRuntime, RuntimeOffsets::runtimeHadesCompacteeStart));
  a.cmp(xScratch, RuntimeOffsets::kHadesNoCompactee);
  a.b_ne(slowLab);

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
  // further in, `loc & ~(unit-1)` names a later unit and the halfword below
  // is object payload. Callers bound `loc`: PutById through
  // WritePropertyCacheEntry::kMaxSlot, array stores through a storage-size
  // gate. RuntimeOffsets::segmentShiftedSize.
  a.ldrh(xScratch.w(), a64::Mem(t1, RuntimeOffsets::segmentShiftedSize));
  a.cmp(xScratch.w(), 1);
  a.b_ne(slowLab);

  // The barrier is now known to be at most a card-dirty. Store. The slot is
  // one SmallHermesValue wide -- four bytes under compressed pointers, eight
  // otherwise -- which is why this goes through emit_store_shv() rather than
  // a bare str.
  emit_store_shv(a, shv, a64::Mem(loc));

  // relocationWriteBarrier() is only reached for a pointer value. The test is
  // made on the ORIGINAL HermesValue rather than on what was just stored,
  // which is both cheaper and more accurate: a HermesValue carries its
  // pointer uncompressed, which is what the segment compare below needs, and
  // the one pointer a SmallHermesValue has that a HermesValue does not -- a
  // BoxedDouble -- can never be here, because the encode declined it. \p t2
  // is dead once the store above has consumed it.
  emit_sh_ljs_get_tag(a, t2, value);
  emit_sh_ljs_tag_is_pointer(a, t2);
  a.b_lo(doneLab);

  // t2 = the start of the segment containing the pointed-to cell. Only an
  // old-to-young pointer needs a card; old-to-old does not, and the compactee
  // cases were declined above. The young-gen start comes back into xScratch,
  // the same non-allocated scratch the tests above used.
  emit_sh_ljs_get_pointer(a, t2, value);
  a.and_(t2, t2, kSegmentMask);
  a.ldr(xScratch, a64::Mem(xRuntime, RuntimeOffsets::runtimeHadesYGStart));
  a.cmp(t2, xScratch);
  a.b_ne(doneLab);

  // FixedSizeHeapSegment::dirtyCardForAddress(loc), inline:
  //   segLoc[(loc - segLoc) >> kLogCardSize] = CardStatus::Dirty
  // The card array base is the segment start itself, since
  // RuntimeOffsets::segmentInlineCards is 0, so the store needs no
  // displacement on top of the base+index addressing. The runtime writes this
  // byte with a relaxed atomic store, which on arm64 is this same
  // instruction.
  static_assert(
      RuntimeOffsets::segmentInlineCards == 0,
      "the card store indexes off the segment start with no displacement");
  a.sub(t2, loc, t1);
  a.lsr(t2, t2, RuntimeOffsets::kLogCardSize);
  a.mov(xScratch.w(), RuntimeOffsets::kCardDirty);
  a.strb(xScratch.w(), a64::Mem(t1, t2));
  a.b(doneLab);

  a.bind(youngTargetLab);
  emit_store_shv(a, shv, a64::Mem(loc));

  a.bind(doneLab);
}
#endif // HERMES_JIT_INLINE_SAFE_STORE

} // namespace hermes::vm::arm64

#endif // HERMESVM_JIT_ARM64

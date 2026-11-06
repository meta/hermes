/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JIT/Config.h"
#if HERMESVM_JIT_ARM64
#include "../JitHandlers.h"
#include "JitEmitter-internal.h"
#include "JitEmitter.h"

#include "hermes/VM/JSObject-inline.h"
#include "llvh/ADT/Statistic.h"

#include <algorithm>

#define DEBUG_TYPE "jit"

STATISTIC(
    JITNumGetByIdSpec,
    "JITNumGetByIdSpec: number of GetById specialized fast paths emitted");

namespace hermes::vm::arm64 {

#if HERMES_JIT_INLINE_SAFE_STORE
void Emitter::emitPutByValFastArrayTier(
    FR frTarget,
    FR frKey,
    FR frValue,
    const asmjit::Label &helperLab,
    bool targetKnownObject) {
  comment("// Inline fast array store");

  // All three operands are already synced to the frame by the caller, so any
  // temp holding another FR is dead weight here.
  freeAllFRTempExcept(frTarget);

  // The target and the value are needed as raw HermesValues in GP registers.
  // The key is needed as the double it has to be, which is what
  // emit_double_is_uint32() consumes; getOrAllocFRInVecD() only moves the
  // raw 64 bits -- fmov or ldr, never a conversion -- so this is safe for a
  // key of any type: a non-number is NaN-encoded and the conversion below
  // rejects it, which is precisely how toArrayIndexFastPath() itself gets
  // away with reading key->f64 unconditionally.
  static_assert(
      HERMESVALUE_VERSION == 2,
      "non-numbers must be NaN-encoded for the key test below");
  HWReg hwTarget = getOrAllocFRInGpX(frTarget, true);
  HWReg hwValue = getOrAllocFRInGpX(frValue, true);
  HWReg hwKey = getOrAllocFRInVecD(frKey, true);

  // As in emitPutByIdInlineTier(), the allocs and frees below emit no code at
  // all: they only decide which registers this sequence may use. Once every
  // register is recorded, all of them are marked free, which is what leaves
  // the helper path with no temp registered as an FR location.
  HWReg hwLoc = allocTempGpX();
  HWReg hwIdx = allocTempGpX();
  HWReg hwTemp1 = allocTempGpX();
  HWReg hwTemp2 = allocTempGpX();
  HWReg hwKeyTmp = allocTempVecD();
  const a64::GpX xTarget = hwTarget.a64GpX();
  const a64::GpX xValue = hwValue.a64GpX();
  const a64::VecD dKey = hwKey.a64VecD();
  // Holds the object, then the indexed storage, then the element address.
  const a64::GpX xLoc = hwLoc.a64GpX();
  const a64::GpX xIdx = hwIdx.a64GpX();
  const a64::GpX xTemp1 = hwTemp1.a64GpX();
  const a64::GpX xTemp2 = hwTemp2.a64GpX();
  const a64::VecD dKeyTmp = hwKeyTmp.a64VecD();
  freeReg(hwLoc);
  freeReg(hwIdx);
  freeReg(hwTemp1);
  freeReg(hwTemp2);
  freeReg(hwKeyTmp);
  freeFRTemp(frTarget);
  freeFRTemp(frKey);
  freeFRTemp(frValue);
  assert(dKeyTmp != dKey && "emit_double_is_uint32() needs a distinct temp");

  // Code generation starts here.

  // The value has to be encoded into the SmallHermesValue an element slot
  // actually holds, and one value cannot be: a double that would need a
  // heap-allocated BoxedDouble is declined here. Doing that FIRST, before any
  // guard, is what makes the decline cheap -- it skips the whole chain below
  // rather than running it and then giving up. xTemp2 holds the result from
  // here to the store; no guard touches it, which on arm64 is a property the
  // guards below had to be written for rather than one they had for free (see
  // the xScratch uses). In the default heap-value mode this emits nothing at
  // all -- not even the comment, which the encoder itself writes -- and
  // `xShv` is `xValue`.
  const a64::GpX &xShv =
      emit_shv_encode_for_slot_or_slow(a, xTemp2, xValue, xTemp1, helperLab);

  // Is the target an object? At a duo site the typed-array tier ahead of this
  // one has already proved that, and nothing runs between the two tiers that
  // could change the answer.
  if (!targetKnownObject) {
    emit_sh_ljs_is_object(a, xTemp1, xTarget);
    a.b_ne(helperLab);
  }
  // xLoc is the pointer to the object.
  emit_sh_ljs_get_pointer(a, xLoc, xTarget);

  // Is it a JSArray, and nothing else? The runtime reaches ArrayImpl's
  // haveOwnIndexed/setOwnIndexed through the object's ObjectVTable; the
  // exact kind is what lets this code skip that dispatch. ArrayImplKind
  // would be the wrong test: Arguments is in that range and shares the
  // storage layout, but restricting to JSArray is what the rest of this
  // sequence was verified against.
  // That the kind fits the byte emit_gccell_get_kind() loads is pinned
  // upstream, by GCCell.h's `kNumCellKinds < 256` static_assert; a value that
  // narrow always encodes as a compare immediate.
  emit_gccell_get_kind(a, xTemp1, xLoc);
  a.cmp(xTemp1.w(), (uint32_t)CellKind::JSArrayKind);
  a.b_ne(helperLab);

  // Fast index properties (indexedStorage set, hasIndexLikeProperties clear)
  // and frozen clear, in one masked compare of the hidden class's flags.
  //
  // arm64: the mask is three non-adjacent bits, which is not a run of ones
  // and therefore not an AArch64 logical immediate, so it has to be
  // materialized in a register first. It goes in xScratch, not in a
  // temporary: xTemp2 is carrying the encoded value across every guard in
  // this tier. The value it is compared against is a single bit and encodes
  // directly.
  static_assert(
      RuntimeOffsets::hiddenClassFlags % 2 == 0 &&
          RuntimeOffsets::hiddenClassFlags < maxNaturalBaseOffset(2),
      "the class flags must be reachable by an unsigned-offset LDRH");
  const uint32_t flagsMask = RuntimeOffsets::classFlagsFastArrayMask();
  const uint32_t flagsValue = RuntimeOffsets::classFlagsFastArrayValue();
  assert(
      flagsMask == 0x884 && flagsValue == 0x800 &&
      "unexpected ClassFlags bit layout");
  assert(
      a64::Utils::isAddSubImm(flagsValue) &&
      "the flags value must encode as a compare immediate");
  emit_load_cp(a, xTemp1, a64::Mem(xLoc, offsetof(SHJSObject, clazz)));
  emit_sh_cp_decode_non_null(a, xTemp1);
  a.ldrh(xTemp1.w(), a64::Mem(xTemp1, RuntimeOffsets::hiddenClassFlags));
  a.mov(xScratch.w(), flagsMask);
  a.and_(xTemp1.w(), xTemp1.w(), xScratch.w());
  a.cmp(xTemp1.w(), flagsValue);
  a.b_ne(helperLab);

  // toArrayIndexFastPath(): the key must be a double that survives a round
  // trip through uint32, and must not be 0xFFFFFFFF, which JS arrays do not
  // accept as an index.
  //
  // arm64 needs no second exit for a NaN key, unlike x86-64's parity check:
  // fcmp sets NZCV to 0b0011 for an unordered compare, so Z is clear and the
  // b.ne below already takes it. A key too large, negative or fractional
  // fails the same comparison, fcvtzu having saturated or truncated it.
  emit_double_is_uint32(a, xIdx.w(), dKeyTmp, dKey);
  a.b_ne(helperLab);
  // idx == 0xFFFFFFFF, as one instruction: cmn adds, so Z is set exactly when
  // idx + 1 wraps to zero.
  a.cmn(xIdx.w(), 1);
  a.b_eq(helperLab);

  // _haveOwnIndexedImpl()'s and _setOwnIndexedImpl()'s range test:
  // `index - beginIndex_ < elemCount_`, unsigned, which is one comparison
  // for both ends. xIdx becomes the index into the storage. The 32-bit
  // subtract zeroes the upper half of xIdx, which is what makes the scaled
  // 64-bit index below the uint32 one.
  static_assert(
      RuntimeOffsets::arrayImplBeginIndex % 4 == 0 &&
          RuntimeOffsets::arrayImplElemCount % 4 == 0 &&
          RuntimeOffsets::arrayImplBeginIndex < maxNaturalBaseOffset(4) &&
          RuntimeOffsets::arrayImplElemCount < maxNaturalBaseOffset(4),
      "the array range fields must be reachable by unsigned-offset LDRs");
  a.ldr(xTemp1.w(), a64::Mem(xLoc, RuntimeOffsets::arrayImplBeginIndex));
  a.sub(xIdx.w(), xIdx.w(), xTemp1.w());
  a.ldr(xTemp1.w(), a64::Mem(xLoc, RuntimeOffsets::arrayImplElemCount));
  a.cmp(xIdx.w(), xTemp1.w());
  a.b_hs(helperLab);

  // The storage. It cannot be null here: elemCount_ is 0 whenever
  // indexedStorage_ is, which the comparison above has already ruled out --
  // the same reasoning that lets _setOwnIndexedImpl() call
  // getIndexedStorageUnsafe() on this path.
  static_assert(
      RuntimeOffsets::arrayImplIndexedStorage % sizeof(CompressedPointer) ==
              0 &&
          RuntimeOffsets::arrayImplIndexedStorage <
              maxNaturalBaseOffset(sizeof(CompressedPointer)),
      "the indexed storage must be reachable by an unsigned-offset LDR");
  emit_load_cp(
      a, xLoc, a64::Mem(xLoc, RuntimeOffsets::arrayImplIndexedStorage));
  emit_sh_cp_decode_non_null(a, xLoc);

  // The storage cell must not be a jumbo cell, or emitSafeStoreOrSlow()'s
  // precondition (see its declaration) is violated. A cell allocated in a
  // JumboHeapSegment has a size greater than kMaxInlineStorage, or 0 if it
  // is too large to be represented at all, so a single unsigned compare of
  // `size - 1` rejects both.
  //
  // KindAndSize packs the size below the kind, in a word as wide as a
  // compressed pointer, so the size occupies the low 32 bits of an 8-byte
  // header and only the low 24 of a 4-byte one. The 32-bit load is right in
  // both cases; the narrow one needs the kind masked off first.
  static_assert(
      RuntimeOffsets::kindAndSizeNumSizeBits <= 32,
      "the size field must fit the 32-bit load below");
  static_assert(
      offsetof(SHGCCell, kindAndSize) % 4 == 0 &&
          offsetof(SHGCCell, kindAndSize) < maxNaturalBaseOffset(4),
      "the cell header must be reachable by an unsigned-offset 32-bit LDR");
  a.ldr(xTemp1.w(), a64::Mem(xLoc, offsetof(SHGCCell, kindAndSize)));
  if constexpr (RuntimeOffsets::kindAndSizeNumSizeBits < 32) {
    constexpr uint32_t kSizeMask =
        (uint32_t)(((uint64_t)1 << RuntimeOffsets::kindAndSizeNumSizeBits) - 1);
    // A run of low ones is a logical immediate, so this needs no scratch.
    assert(
        a64::Utils::isLogicalImm(kSizeMask, 32) &&
        "the size mask must encode as a logical immediate");
    a.and_(xTemp1.w(), xTemp1.w(), kSizeMask);
  }
  a.sub(xTemp1.w(), xTemp1.w(), 1);
  emit_cmp_imm32(
      a, xTemp1.w(), RuntimeOffsets::kMaxInlineStorage - 1, xScratch.w());
  a.b_hi(helperLab);

  // The address of the element. The scale is the width of a heap value slot,
  // which is four bytes under compressed pointers and eight otherwise -- an
  // element index is not a byte offset in any mode.
  a.add(xLoc, xLoc, xIdx, a64::lsl(RuntimeOffsets::kLogSmallHermesValueSize));
  static_assert(
      offsetof(SHArrayStorageSmall, storage) <= 0xFFFFFF,
      "the element base offset must fit emit_add_imm_u24()");
  emit_add_imm_u24(a, xLoc, offsetof(SHArrayStorageSmall, storage));

  // _haveOwnIndexedImpl() reports false for an `empty` element, so a write
  // over a hole is not a fast-path write at all: the runtime resolves the
  // property normally, and may find an accessor on the prototype chain.
  // Decline, and let the helper do that.
  //
  // Where a slot is eight bytes it holds the HermesValue's bits unshifted, so
  // the test is HermesValue's own ETag test; where it is four the bits have
  // been shifted down out of ETag position and the whole encoded empty value
  // is compared instead. emit_shv_load_is_empty() picks between the two.
  emit_shv_load_is_empty(a, xTemp1, a64::Mem(xLoc));
  a.b_eq(helperLab);

  // Store, if the write barrier for it is a no-op or a card-dirty.
  emitSafeStoreOrSlow(xLoc, xShv, xValue, xTemp1, xTemp2, helperLab);
}
#endif // HERMES_JIT_INLINE_SAFE_STORE

void Emitter::emitPutByValTypedArrayTier(
    FR frTarget,
    FR frKey,
    FR frValue,
    CellKind kind,
    const asmjit::Label &kindMissLab,
    const asmjit::Label &helperLab) {
  comment("// Inline typed array store (kind %u)", (unsigned)kind);

  const bool isFloat64 = kind == CellKind::Float64ArrayKind;
  const bool isFloat32 = kind == CellKind::Float32ArrayKind;
  // The element width, as a log2 byte count: also the scale of the store's
  // index operand, since the index is an element count.
  uint32_t logWidth;
  switch (kind) {
    case CellKind::Uint8ArrayKind:
    case CellKind::Int8ArrayKind:
      logWidth = 0;
      break;
    case CellKind::Uint16ArrayKind:
    case CellKind::Int16ArrayKind:
      logWidth = 1;
      break;
    case CellKind::Uint32ArrayKind:
    case CellKind::Int32ArrayKind:
    case CellKind::Float32ArrayKind:
      logWidth = 2;
      break;
    case CellKind::Float64ArrayKind:
      logWidth = 3;
      break;
    default:
      llvm_unreachable("unsupported typed-array tier kind");
  }

  // All three operands are already synced to the frame by the caller, so any
  // temp holding another FR is dead weight here.
  freeAllFRTempExcept(frTarget);

  // The target is needed as a raw HermesValue in a GP register. The key and
  // the value are needed as the doubles they have to be; getOrAllocFRInVecD()
  // only moves the raw 64 bits -- fmov or ldr, never a conversion -- so this
  // is safe for an operand of any type: a non-number is NaN-encoded, and both
  // the key test and the value conversion below reject every NaN-encoded
  // pattern.
  static_assert(
      HERMESVALUE_VERSION == 2,
      "non-numbers must be NaN-encoded for the tests below");
  HWReg hwTarget = getOrAllocFRInGpX(frTarget, true);
  HWReg hwValue = getOrAllocFRInVecD(frValue, true);
  HWReg hwKey = getOrAllocFRInVecD(frKey, true);

  // As in emitPutByValFastArrayTier(), the allocs and frees below emit no
  // code: they only decide which registers this sequence may use, and then
  // leave the helper path with no temp registered as an FR location. The
  // third vector temp is allocated for every kind even though the integer
  // kinds and Float32 use a different pair of the three: uniform allocation
  // keeps this prologue one shape.
  HWReg hwLoc = allocTempGpX();
  HWReg hwIdx = allocTempGpX();
  HWReg hwTemp1 = allocTempGpX();
  HWReg hwTemp2 = allocTempGpX();
  HWReg hwKeyTmp = allocTempVecD();
  HWReg hwValTmp = allocTempVecD();
  HWReg hwLimit = allocTempVecD();
  const a64::GpX xTarget = hwTarget.a64GpX();
  const a64::VecD dValue = hwValue.a64VecD();
  const a64::VecD dKey = hwKey.a64VecD();
  // Holds the object, then the buffer, then the element base address.
  const a64::GpX xLoc = hwLoc.a64GpX();
  const a64::GpX xIdx = hwIdx.a64GpX();
  const a64::GpX xTemp1 = hwTemp1.a64GpX();
  // Holds the truncated integer value from the conversion to the store.
  const a64::GpX xTemp2 = hwTemp2.a64GpX();
  const a64::VecD dKeyTmp = hwKeyTmp.a64VecD();
  // For the integer kinds the magnitude of the value; for Float32 the value
  // narrowed to single precision, which is stored through its S view.
  const a64::VecD dValTmp = hwValTmp.a64VecD();
  const a64::VecS sValTmp{hwValTmp.indexInClass()};
  // The integer kinds' magnitude limit, 2^63 as a double.
  const a64::VecD d2p63 = hwLimit.a64VecD();
  freeReg(hwLoc);
  freeReg(hwIdx);
  freeReg(hwTemp1);
  freeReg(hwTemp2);
  freeReg(hwKeyTmp);
  freeReg(hwValTmp);
  freeReg(hwLimit);
  freeFRTemp(frTarget);
  freeFRTemp(frKey);
  freeFRTemp(frValue);
  assert(dKeyTmp != dKey && "emit_double_is_uint32() needs a distinct temp");
  assert(
      dValTmp != dValue && d2p63 != dValue &&
      "the value conversion needs temps distinct from the value");

  // Code generation starts here.

  // The object and exact-kind checks come FIRST, before any value-based
  // decline: at a duo site a kind miss must reach the JSArray tier, which
  // handles the non-number values the checks below would decline.
  emit_sh_ljs_is_object(a, xTemp1, xTarget);
  a.b_ne(helperLab);
  emit_sh_ljs_get_pointer(a, xLoc, xTarget);
  // That the kind fits the byte emit_gccell_get_kind() loads is pinned
  // upstream, by GCCell.h's `kNumCellKinds < 256` static_assert; a value that
  // narrow always encodes as a compare immediate.
  emit_gccell_get_kind(a, xTemp1, xLoc);
  a.cmp(xTemp1.w(), (uint32_t)kind);
  a.b_ne(kindMissLab);

  // Class flags: fast index properties (indexedStorage set,
  // hasIndexLikeProperties clear), frozen clear -- the same masked compare
  // the fast array tier emits, and for the same reason. An out-of-range
  // defineProperty() gives the class index-like properties, and a frozen
  // typed array must throw on a strict store rather than be written.
  //
  // arm64: the mask is three non-adjacent bits, which is not an AArch64
  // logical immediate, so it has to be materialized in a register first.
  // xScratch is the right place for it: nothing in this tier holds a value
  // there, and nothing else is live in it across these instructions.
  static_assert(
      RuntimeOffsets::hiddenClassFlags % 2 == 0 &&
          RuntimeOffsets::hiddenClassFlags < maxNaturalBaseOffset(2),
      "the class flags must be reachable by an unsigned-offset LDRH");
  const uint32_t flagsMask = RuntimeOffsets::classFlagsFastArrayMask();
  const uint32_t flagsValue = RuntimeOffsets::classFlagsFastArrayValue();
  assert(
      flagsMask == 0x884 && flagsValue == 0x800 &&
      "unexpected ClassFlags bit layout");
  assert(
      a64::Utils::isAddSubImm(flagsValue) &&
      "the flags value must encode as a compare immediate");
  emit_load_cp(a, xTemp1, a64::Mem(xLoc, offsetof(SHJSObject, clazz)));
  emit_sh_cp_decode_non_null(a, xTemp1);
  a.ldrh(xTemp1.w(), a64::Mem(xTemp1, RuntimeOffsets::hiddenClassFlags));
  a.mov(xScratch.w(), flagsMask);
  a.and_(xTemp1.w(), xTemp1.w(), xScratch.w());
  a.cmp(xTemp1.w(), flagsValue);
  a.b_ne(helperLab);

  // The value conversion, which is also the "is it a number this element type
  // can hold" guard. Non-numbers are NaN-encoded, so:
  //  - integer kinds: two exits admit exactly the finite doubles in
  //    (-2^63, +2^63), and fcvtzs truncates every one of them correctly --
  //    discarding the fraction, as truncateToInt32() requires -- so the low
  //    bits of the result are its modular answer. An unordered fcmp (V set)
  //    declines every NaN, i.e. every non-number; comparing the MAGNITUDE
  //    against 2^63 declines both infinities and everything at or beyond the
  //    limit, which is where fcvtzs would saturate instead of truncating.
  //    That admitted set is deliberately the same one x86-64's integer
  //    conversion admits (it declines NaN, either infinity, |x| >= 2^63 and
  //    -2^63 via its sentinel compare): the two backends must decline the
  //    same values, or a site fed values one accepts and the other does not
  //    would demote on one backend and not the other.
  //  - float kinds: the same unordered exit declines every NaN bit pattern, a
  //    real NaN value included, because the helper stores the canonical NaN
  //    and these raw bits need not be it. This is sound only because a
  //    non-number is always a NaN and never an infinity: the lowest tag is
  //    HVTag_First == 0xf9, so bits 51:48 of any tagged value are at least 9,
  //    i.e. its mantissa is never zero.
  a.fcmp(dValue, dValue);
  a.b_vs(helperLab);
  if (isFloat64) {
    // Stored as-is, out of its own register.
  } else if (isFloat32) {
    a.fcvt(sValTmp, dValue);
  } else {
    // 2^63 as a double. Materialized once, here, for the whole tier; a single
    // MOVZ, since only bits 63:48 of the pattern are nonzero. xTemp1 is dead
    // between the flags check above and the bounds check below.
    loadBits64InGp(xTemp1, UINT64_C(0x43E0000000000000), "double 2^63");
    a.fmov(d2p63, xTemp1);
    a.fabs(dValTmp, dValue);
    a.fcmp(dValTmp, d2p63);
    a.b_ge(helperLab);
    a.fcvtzs(xTemp2, dValue);
  }

  // The key must be a double that converts to a uint32 and back unchanged.
  // As in the fast array tier one exit covers every rejection, a NaN key
  // included: an unordered fcmp leaves Z clear, so b.ne takes it. Unlike JS
  // arrays, typed arrays need no 0xFFFFFFFF exclusion -- the bounds check
  // below rejects it.
  emit_double_is_uint32(a, xIdx.w(), dKeyTmp, dKey);
  a.b_ne(helperLab);

  // Bounds: idx < length_, one unsigned 32-bit compare. This tree has no
  // resizable ArrayBuffers, so length_ is fixed for the object's lifetime.
  static_assert(
      RuntimeOffsets::jsTypedArrayBaseLength % 4 == 0 &&
          RuntimeOffsets::jsTypedArrayBaseLength < maxNaturalBaseOffset(4),
      "the typed array length must be reachable by an unsigned-offset LDR");
  a.ldr(xTemp1.w(), a64::Mem(xLoc, RuntimeOffsets::jsTypedArrayBaseLength));
  a.cmp(xIdx.w(), xTemp1.w());
  a.b_hs(helperLab);

  // The element base address: the buffer's data_, plus the view's byte
  // offset_ into it. A detached buffer has a null data_, so the attached
  // check is free with a load the store needs anyway. The offset goes in a
  // real temp, NOT in xScratch: on arm64 xScratch is what mask and immediate
  // materializations use, so nothing may live there across other emitters.
  // The 32-bit load zeroes the upper half of xTemp1, which is what makes the
  // 64-bit add below the zero-extended one.
  static_assert(
      RuntimeOffsets::jsTypedArrayBaseOffset % 4 == 0 &&
          RuntimeOffsets::jsTypedArrayBaseOffset < maxNaturalBaseOffset(4),
      "the typed array offset must be reachable by an unsigned-offset LDR");
  static_assert(
      RuntimeOffsets::jsTypedArrayBaseBuffer % sizeof(CompressedPointer) == 0 &&
          RuntimeOffsets::jsTypedArrayBaseBuffer <
              maxNaturalBaseOffset(sizeof(CompressedPointer)),
      "the typed array buffer must be reachable by an unsigned-offset LDR");
  static_assert(
      RuntimeOffsets::jsArrayBufferData % 8 == 0 &&
          RuntimeOffsets::jsArrayBufferData < maxNaturalBaseOffset(8),
      "the buffer data pointer must be reachable by an unsigned-offset LDR");
  a.ldr(xTemp1.w(), a64::Mem(xLoc, RuntimeOffsets::jsTypedArrayBaseOffset));
  emit_load_cp(a, xLoc, a64::Mem(xLoc, RuntimeOffsets::jsTypedArrayBaseBuffer));
  emit_sh_cp_decode_non_null(a, xLoc);
  a.ldr(xLoc, a64::Mem(xLoc, RuntimeOffsets::jsArrayBufferData));
  a.cbz(xLoc, helperLab);
  a.add(xLoc, xLoc, xTemp1);

  // The store. The index is an ELEMENT count, so it is scaled by the element
  // width; emit_double_is_uint32() leaves it zero-extended, which is what the
  // UXTW index form wants.
  const a64::Mem elem{xLoc, xIdx.w(), a64::uxtw(logWidth)};
  if (isFloat64) {
    a.str(dValue, elem);
  } else if (isFloat32) {
    a.str(sValTmp, elem);
  } else if (logWidth == 0) {
    a.strb(xTemp2.w(), elem);
  } else if (logWidth == 1) {
    a.strh(xTemp2.w(), elem);
  } else {
    a.str(xTemp2.w(), elem);
  }
}

void Emitter::putByValImpl(
    FR frTarget,
    FR frKey,
    FR frValue,
    const char *name,
    bool strict) {
  // The PutByVal instruction's own bytecode offset, unique within this
  // function and stable across recompiles: identifies this site's entry
  // in versionData_'s consumer records (see JitFunctionData.h).
  uint32_t siteId =
      (uint32_t)((const char *)emittingIP - (const char *)codeBlock_->begin());

  comment(
      "// %s r%u, r%u, r%u",
      name,
      frTarget.index(),
      frKey.index(),
      frValue.index());

  syncAllFRTempExcept({});
  syncToFrame(frTarget);
  syncToFrame(frKey);
  syncToFrame(frValue);

  // Monotone tier selection (spec: "Monotone emission: tiers are only ever
  // added"). Unlike the PutById tier these need nothing from a property
  // cache: the guards are all on the values themselves, so the tiers go
  // ahead of the helper call unconditionally. PutByVal passes the target as
  // the receiver, which is the one precondition of the runtime fast path
  // that is satisfied by construction rather than by a guard.
  //
  // The JSArray tier is a static prior, not a decision: it is emitted at
  // every site this build can emit one at, in every version, exactly the
  // pre-feedback behavior. Nothing observes its hits, so no evidence about
  // it could ever be honest; dropping it is what made a pure-JSArray
  // function pay a permanent instrumentation tax.
  //
  // The typed-array tier is emitted iff the record holds an observed
  // supported kind. taKind never un-learns (a second kind sets taPoisoned
  // and leaves the first in place), so selecting from the observed field
  // alone is already monotone: no previously-emitted set needs consulting,
  // and a poisoned site keeps running its first kind's traffic inline.
  JitByValSiteRecord &site = byValSiteRecord(siteId);
#if HERMES_JIT_INLINE_SAFE_STORE
  const bool emitJSArray = true;
#else
  // The fast array tier needs the inline write barrier, which this build does
  // not have; the typed-array tier stores no GC pointers and remains
  // available.
  const bool emitJSArray = false;
#endif
  const bool emitTA = site.taKind != JitByValSiteRecord::kTAKindNone &&
      isJitSupportedTypedArrayStoreKind((CellKind)site.taKind);
  site.specializedTAKind =
      emitTA ? site.taKind : JitByValSiteRecord::kTAKindNone;

  asmjit::Label helperLab{};
  asmjit::Label contLab{};
  if (emitJSArray || emitTA) {
    helperLab = a.newLabel();
    contLab = a.newLabel();
  }
  if (emitTA) {
    // The recorded kind is compared first: it is what the site was observed
    // declining on. At a duo site its kind miss chains into the JSArray tier
    // rather than into the helper.
    asmjit::Label kindMissLab = emitJSArray ? a.newLabel() : helperLab;
    emitPutByValTypedArrayTier(
        frTarget,
        frKey,
        frValue,
        (CellKind)site.taKind,
        kindMissLab,
        helperLab);
    // Falling out of the inline tier means the store is done.
    a.b(contLab);
    if (emitJSArray)
      a.bind(kindMissLab);
  }
#if HERMES_JIT_INLINE_SAFE_STORE
  if (emitJSArray) {
    emitPutByValFastArrayTier(
        frTarget,
        frKey,
        frValue,
        helperLab,
        /* targetKnownObject */ emitTA);
    a.b(contLab);
  }
#endif
  if (emitJSArray || emitTA)
    a.bind(helperLab);

  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frTarget);
  loadFrameAddr(a64::x2, frKey);
  loadFrameAddr(a64::x3, frValue);
  loadBits64InGp(a64::x4, (uint64_t)versionData_, "JitVersionData");
  a.mov(a64::w5, siteId);

  // site.helper is per-site mutable state (spec: "Slow-path demotion: the
  // pointer flip"): it starts out null on a fresh record and is carried
  // forward as-is across recompiles, so a demoted slot (flipped by the
  // runtime to the matching plain _sh_ljs helper) stays demoted. Only a
  // still-null slot -- never yet flipped -- is initialized here, to the
  // recording helper matching this instruction's strictness.
  if (!site.helper) {
    site.helper =
        strict ? (void *)_jit_put_by_val_strict : (void *)_jit_put_by_val_loose;
  }
  // Type-check both possible recording callees against the signature the
  // emitted call sequence assumes, the same way EMIT_RUNTIME_CALL does for
  // a direct call -- even though neither is called directly here, since
  // the actual callee is loaded from site.helper at runtime.
  using _FnT = void (*)(
      SHRuntime *,
      SHLegacyValue *,
      SHLegacyValue *,
      SHLegacyValue *,
      SHJitVersionData *,
      uint32_t);
  _FnT _fn = strict ? _jit_put_by_val_strict : _jit_put_by_val_loose;
  (void)_fn;
  callRuntimeWithSavedIPIndirect(
      (uint64_t)&site.helper,
      strict ? "_jit_put_by_val_strict [indirect]"
             : "_jit_put_by_val_loose [indirect]");

  if (emitJSArray || emitTA)
    a.bind(contLab);
}

void Emitter::putByValWithReceiver(
    FR frTarget,
    FR frKey,
    FR frValue,
    FR frReceiver,
    bool isStrict) {
  comment(
      "// PutByValWithReceiver r%u, r%u, r%u, r%u, %d",
      frTarget.index(),
      frKey.index(),
      frValue.index(),
      frReceiver.index(),
      isStrict);

  syncAllFRTempExcept({});
  syncToFrame(frTarget);
  syncToFrame(frKey);
  syncToFrame(frValue);
  syncToFrame(frReceiver);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frTarget);
  loadFrameAddr(a64::x2, frKey);
  loadFrameAddr(a64::x3, frValue);
  loadFrameAddr(a64::x4, frReceiver);
  a.mov(a64::w5, isStrict);
  EMIT_RUNTIME_CALL(
      *this,
      void (*)(
          SHRuntime *shr,
          SHLegacyValue *target,
          SHLegacyValue *key,
          SHLegacyValue *value,
          SHLegacyValue *receiver,
          bool isStrict),
      _sh_ljs_put_by_val_with_receiver_rjs);
}

void Emitter::delByVal(FR frRes, FR frTarget, FR frKey, bool strict) {
  comment(
      "// DelByVal r%u, r%u, r%u, %d",
      frRes.index(),
      frTarget.index(),
      frKey.index(),
      strict);

  syncAllFRTempExcept(frRes != frTarget && frRes != frKey ? frRes : FR{});
  syncToFrame(frTarget);
  syncToFrame(frKey);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frTarget);
  loadFrameAddr(a64::x2, frKey);
  if (strict) {
    EMIT_RUNTIME_CALL(
        *this,
        SHLegacyValue (*)(SHRuntime *, SHLegacyValue *, SHLegacyValue *),
        _sh_ljs_del_by_val_strict);
  } else {
    EMIT_RUNTIME_CALL(
        *this,
        SHLegacyValue (*)(SHRuntime *, SHLegacyValue *, SHLegacyValue *),
        _sh_ljs_del_by_val_loose);
  }

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::addOwnPrivateBySym(FR frTarget, FR frKey, FR frValue) {
  comment(
      "// AddOwnPrivateBySym r%u, r%u, r%u",
      frTarget.index(),
      frKey.index(),
      frValue.index());

  syncAllFRTempExcept({});
  syncToFrame(frTarget);
  syncToFrame(frKey);
  syncToFrame(frValue);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frTarget);
  loadFrameAddr(a64::x2, frKey);
  loadFrameAddr(a64::x3, frValue);
  EMIT_RUNTIME_CALL(
      *this,
      void (*)(SHRuntime *, SHLegacyValue *, SHLegacyValue *, SHLegacyValue *),
      _sh_ljs_add_own_private_by_sym);
}

void Emitter::getOwnPrivateBySym(
    FR frRes,
    FR frTarget,
    FR frKey,
    uint8_t cacheIdx) {
  comment(
      "// GetOwnPrivateBySym r%u, r%u, r%u, cache %u",
      frRes.index(),
      frTarget.index(),
      frKey.index(),
      cacheIdx);

  syncAllFRTempExcept(frRes != frTarget && frRes != frKey ? frRes : FR());
  syncToFrame(frTarget);
  syncToFrame(frKey);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frTarget);
  loadFrameAddr(a64::x2, frKey);

  if (cacheIdx == hbc::PROPERTY_CACHING_DISABLED) {
    a.mov(a64::x3, 0);
  } else {
    a.ldr(a64::x3, a64::Mem(roDataLabel_, roOfsPrivateNameCachePtr_));
    if (cacheIdx != 0)
      a.add(a64::x3, a64::x3, sizeof(SHPrivateNameCacheEntry) * cacheIdx);
  }

  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(
          SHRuntime *,
          const SHLegacyValue *,
          const SHLegacyValue *,
          SHPrivateNameCacheEntry *),
      _sh_ljs_get_own_private_by_sym);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}
void Emitter::putOwnPrivateBySym(
    FR frTarget,
    FR frKey,
    FR frValue,
    uint8_t cacheIdx) {
  comment(
      "// PutOwnPrivateBySym r%u, r%u, r%u, cache %u",
      frTarget.index(),
      frKey.index(),
      frValue.index(),
      cacheIdx);

  syncAllFRTempExcept({});
  syncToFrame(frTarget);
  syncToFrame(frKey);
  syncToFrame(frValue);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frTarget);
  loadFrameAddr(a64::x2, frKey);
  loadFrameAddr(a64::x3, frValue);

  if (cacheIdx == hbc::PROPERTY_CACHING_DISABLED) {
    a.mov(a64::x4, 0);
  } else {
    a.ldr(a64::x4, a64::Mem(roDataLabel_, roOfsPrivateNameCachePtr_));
    if (cacheIdx != 0)
      a.add(a64::x4, a64::x4, sizeof(SHPrivateNameCacheEntry) * cacheIdx);
  }

  EMIT_RUNTIME_CALL(
      *this,
      void (*)(
          SHRuntime *,
          SHLegacyValue *,
          SHLegacyValue *,
          SHLegacyValue *,
          SHPrivateNameCacheEntry *),
      _sh_ljs_put_own_private_by_sym);
}

// GetById chain entries load the full parent cache epoch with LDR's
// unsigned-offset form, whose immediate is a 12-bit multiple of 8. The
// offset is an offsetof() into Runtime, which grows: pin it so that
// outgrowing the encoding breaks the build rather than every compile.
static_assert(
    RuntimeOffsets::runtimeParentCacheEpoch % 8 == 0 &&
        RuntimeOffsets::runtimeParentCacheEpoch < kMaxInlineBaseOffset,
    "the parent cache epoch must be reachable by an unsigned-offset LDR");

class HERMES_ATTRIBUTE_INTERNAL_LINKAGE Emitter::GetByIdImpl {
  Emitter &_;
  a64::Assembler &a;

  FR frRes;
  SHSymbolID symID;
  FR frSource;
  uint8_t cacheIdx;
  const char *name;
  SHLegacyValue (*shImpl)(
      SHRuntime *shr,
      const SHLegacyValue *source,
      SHSymbolID symID,
      SHReadPropertyCacheEntry *propCacheEntry);
  const char *shImplName;
  /// Whether this instruction belongs to the GetById family, which
  /// records shapes through its site's helper slot.
  bool recording_;
  /// The candidate version's record for this site; null unless
  /// recording_ and the instruction has a property cache entry.
  JitByIdSiteRecord *site_ = nullptr;

  /// The entries emitPolyChain() emits: own entries, then chain entries,
  /// then prototype entries.
  llvh::SmallVector<JitByIdPolyEntry, JitByIdSiteRecord::kMaxEntries> poly_;

  asmjit::Label contLab;
  asmjit::Label slowPathLab;
  HWReg hwRes;
  a64::GpX xTemp1;
  a64::GpX xTemp2;
  a64::GpX xTemp3;
  a64::GpX xTemp4;

 public:
  GetByIdImpl(
      Emitter &emitter,
      FR frRes,
      SHSymbolID symID,
      FR frSource,
      uint8_t cacheIdx,
      const char *name,
      SHLegacyValue (*shImpl)(
          SHRuntime *shr,
          const SHLegacyValue *source,
          SHSymbolID symID,
          SHReadPropertyCacheEntry *propCacheEntry),
      const char *shImplName,
      bool recording)
      : _(emitter),
        a(emitter.a),
        frRes(frRes),
        symID(symID),
        frSource(frSource),
        cacheIdx(cacheIdx),
        name(name),
        shImpl(shImpl),
        shImplName(shImplName),
        recording_(recording) {}

  void run() {
    _.comment(
        "// %s r%u, r%u, cache %u, symID %u",
        name,
        frRes.index(),
        frSource.index(),
        cacheIdx,
        symID);

    if (recording_ && cacheIdx != hbc::PROPERTY_CACHING_DISABLED) {
      uint32_t siteId = (uint32_t)((const char *)_.emittingIP -
                                   (const char *)_.codeBlock_->begin());
      site_ = &_.byIdSiteRecord(siteId);
      // Only a never-flipped slot is initialized; a demoted value
      // carried forward stays demoted.
      if (!site_->helper)
        site_->helper = (void *)_jit_get_by_id;
    }

    // All temporaries will potentially be clobbered by the slow path.
    _.syncAllFRTempExcept(frRes != frSource ? frRes : FR{});
    // Ensure the source register is in memory for the slow path.
    _.syncToFrame(frSource);

    if (cacheIdx != hbc::PROPERTY_CACHING_DISABLED) {
      slowPathLab = a.newLabel();
      contLab = a.newLabel();
      emitFastPath();
      a.bind(slowPathLab);
    } else {
      // All temporaries will be clobbered.
      _.freeAllFRTempExcept({});

      // Remember the result register.
      hwRes = _.getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
    }

    a.mov(a64::x0, xRuntime);
    _.loadFrameAddr(a64::x1, frSource);
    a.mov(a64::w2, symID);
    if (cacheIdx == hbc::PROPERTY_CACHING_DISABLED) {
      a.mov(a64::x3, 0);
    } else {
      a.ldr(a64::x3, a64::Mem(_.roDataLabel_, _.roOfsReadPropertyCachePtr_));
      if (cacheIdx != 0)
        emit_add_imm_u24(
            a, a64::x3, sizeof(SHReadPropertyCacheEntry) * cacheIdx);
    }
    if (site_) {
      _.loadBits64InGp(a64::x4, (uint64_t)_.versionData_, "JitVersionData");
      // x5 is both the helper's record argument and the base of the
      // helper slot the call goes through.
      _.loadBits64InGp(a64::x5, (uint64_t)site_, "JitByIdSiteRecord");
      // Type-check the recording callee against the call sequence; the
      // demoted _sh_ljs_get_by_id_rjs ignores x4/x5 under AAPCS64.
      using _FnT = SHLegacyValue (*)(
          SHRuntime *,
          const SHLegacyValue *,
          SHSymbolID,
          SHReadPropertyCacheEntry *,
          SHJitVersionData *,
          SHJitByIdSiteRecord *);
      _FnT _fn = _jit_get_by_id;
      (void)_fn;
      // Demotion may store _jit_get_by_id_refresh in the slot instead.
      _FnT _fnRefresh = _jit_get_by_id_refresh;
      (void)_fnRefresh;
      // JitByIdSiteRecord is not standard-layout, so the slot's offset
      // comes from the record, not offsetof.
      int32_t helperOfs =
          (int32_t)((const char *)&site_->helper - (const char *)site_);
      // The ldr immediate is scaled by 8 in 12 bits.
      assert(
          helperOfs >= 0 && helperOfs % 8 == 0 && helperOfs < 32768 &&
          "helper slot out of range for a scaled ldr immediate");
      _.callRuntimeWithSavedIPIndirect(
          a64::Mem(a64::x5, helperOfs), "_jit_get_by_id [indirect]");
    } else {
      _.callRuntimeWithSavedIP((void *)shImpl, shImplName);
    }

    _.movHWFromHW<false>(hwRes, HWReg::gpX(0));
    _.frUpdatedWithHW(frRes, hwRes);

    if (contLab.isValid())
      a.bind(contLab);
  }

 private:
  /// Load a SHV from a given slot in an object.
  ///
  /// \param resReg The register to load the SHV into.
  /// \param objReg The register containing the object pointer.
  /// \param tmpReg A temporary register which might be needed if the slot is
  ///   too large to fit in an immediate load. It must not be the same as
  ///   \p objReg, but can be the same as \p resReg.
  /// \param slot The slot index to load from the object.
  void emitLoadFromSlot(
      const a64::GpX &resReg,
      const a64::GpX &objReg,
      const a64::GpX &tmpReg,
      SlotIndex slot) {
    assert(tmpReg != objReg && "tmpReg and objReg must be different");
    if (slot < HERMESVM_DIRECT_PROPERTY_SLOTS) {
      emit_load_from_base_offset<sizeof(SHGCSmallHermesValue), true>(
          a,
          resReg,
          objReg,
          {},
          offsetof(SHJSObjectAndDirectProps, directProps) +
              slot * sizeof(SHGCSmallHermesValue));
    } else {
      emit_load_cp(
          a, objReg, a64::Mem(objReg, offsetof(SHJSObject, propStorage)));
      emit_sh_cp_decode_non_null(a, objReg);
      emit_load_from_base_offset<sizeof(SmallHermesValue), false>(
          a,
          resReg,
          objReg,
          tmpReg,
          offsetof(SHArrayStorageSmall, storage) +
              (slot - HERMESVM_DIRECT_PROPERTY_SLOTS) *
                  sizeof(SHGCSmallHermesValue));
    }
  }

  void emitFastPath() {
    // Label for indirect property access.
    asmjit::Label indirectLab = a.newLabel();

    // We don't need the other temporaries.
    _.freeAllFRTempExcept(frSource);

    // We need the source in a GPx register.
    HWReg hwSourceGpx = _.getOrAllocFRInGpX(frSource, true);

    // Here we start allocating and freeing registers. It is important to
    // realize that this doesn't generate any code, it only updates
    // metadata, marking registers as used or free. So, we have to perform a
    // series of register allocs and frees, ahead of time, based on our
    // understanding of how the live ranges of these registers overlap. Then
    // we just use the recorded registers at the right time.

    // xTemp1 will contain the input object.
    HWReg hwTemp1Gpx = _.allocTempGpX();
    xTemp1 = hwTemp1Gpx.a64GpX();
    // Free frSource before allocating more temporaries, because we won't
    // need it at the same time as them.
    _.freeFRTemp(frSource);

    // Get register assignments for the rest of the temporaries.
    HWReg hwTemp2Gpx = _.allocTempGpX();
    HWReg hwTemp3Gpx = _.allocTempGpX();
    HWReg hwTemp4Gpx = _.allocTempGpX();
    xTemp2 = hwTemp2Gpx.a64GpX();
    xTemp3 = hwTemp3Gpx.a64GpX();
    xTemp4 = hwTemp4Gpx.a64GpX();

    // Now that we have recorded their registers, mark all temp registers as
    // free.
    _.freeReg(hwTemp1Gpx);
    _.freeReg(hwTemp2Gpx);
    _.freeReg(hwTemp3Gpx);
    _.freeReg(hwTemp4Gpx);

    // Allocate the result register. Note that it can overlap the temps we
    // just freed.
    hwRes = _.getOrAllocFRInGpX(frRes, false, HWReg::gpX(0));

    // Finally we begin code generation for the fast path.

    // Is the input an object.
    emit_sh_ljs_is_object(a, xTemp1, hwSourceGpx.a64GpX());
    a.b_ne(slowPathLab);
    // xTemp1 is the pointer to the object.
    emit_sh_ljs_get_pointer(a, xTemp1, hwSourceGpx.a64GpX());

    // xTemp2 is the hidden class.
    emit_load_cp(a, xTemp2, a64::Mem(xTemp1, offsetof(SHJSObject, clazz)));

    Emit_sh_shv_decode shvDecode(a, hwRes.a64GpX(), contLab);

    ReadPropertyCacheEntry *cacheEntry =
        _.codeBlock_->getReadCacheEntry(cacheIdx);
    if (site_ &&
        prepareByIdPolyChain(
            _.runtime_,
            *site_,
            cacheEntry,
            [this](HiddenClass *hc) { return _.initHCLazyIDMayAlloc(hc); },
            poly_)) {
      // Replaces the single-shape choice; always followed by the
      // generic tier below. Not a cold site.
      emitPolyChain(shvDecode);
    } else if (cacheEntry->numGoodChanges == 1) {
      // Optionally emit a very fast path specialized for the cache entry,
      // if the entry had exactly one successful match.
      if (cacheEntry->clazz.getNoBarrierUnsafe() &&
          !cacheEntry->negMatchClazz.getNoBarrierUnsafe()) {
        JITNumGetByIdSpec += emitObjectSpecialization(shvDecode, cacheEntry);
      } else if (
          cacheEntry->clazz.getNoBarrierUnsafe() &&
          cacheEntry->negMatchClazz.getNoBarrierUnsafe()) {
        if (emitParentSpecialization(shvDecode, cacheEntry)) {
          ++JITNumGetByIdSpec;
          // We don't try other things after parent specialization.
          return;
        }
        // If it emitted nothing, fall through to the generic tier below
        // rather than leaving the site with no inline cache at all.
      }
    } else if (!cacheEntry->clazz.getNoBarrierUnsafe()) {
      // Cold cache: no specialization possible yet. A recompile after
      // the cache warms can upgrade this site.
      _.coldReadCacheIdxs_.push_back(cacheIdx);
    }

    _.comment("// Read property cache");

    // xTemp3 points to the start of read property cache.
    a.ldr(xTemp3, a64::Mem(_.roDataLabel_, _.roOfsReadPropertyCachePtr_));
    // xTemp4 = cacheEntry->clazz.
    emit_load_cp(
        a,
        xTemp4,
        a64::Mem(
            xTemp3,
            sizeof(SHReadPropertyCacheEntry) * cacheIdx +
                offsetof(SHReadPropertyCacheEntry, clazz)));

    // Compare hidden classes.
    a.cmp(xTemp2, xTemp4);
    a.b_ne(slowPathLab);

    // Hidden class matches. Fetch the slot in xTemp4
    emit_load_slot16(a, xTemp4, xTemp3, cacheIdx);

    // Is it an indirect slot?
    a.cmp(xTemp4.w(), HERMESVM_DIRECT_PROPERTY_SLOTS);
    a.b_hs(indirectLab);

    // Shift by 2 or 3 bits depending on whether properties are 4 or 8
    // bytes.
    constexpr size_t kPropShiftAmt = sizeof(SHGCSmallHermesValue) == 4 ? 2 : 3;
    // Load from a direct slot.
    a.add(xTemp3, xTemp1, offsetof(SHJSObjectAndDirectProps, directProps));
    emit_load_shv(
        a,
        hwRes.a64GpX(),
        a64::Mem(
            xTemp3, xTemp4, a64::Shift(a64::ShiftOp::kLSL, kPropShiftAmt)));
    shvDecode.emitFirstCase(a);
    a.b(contLab);

    a.bind(indirectLab);
    // Load from an in-direct slot.
    // xTemp1 is the object
    // xTemp4 is the slot

    // xTemp1 = xTemp1->propStorage
    emit_load_cp(
        a, xTemp1, a64::Mem(xTemp1, offsetof(SHJSObject, propStorage)));
    emit_sh_cp_decode_non_null(a, xTemp1);
    constexpr ssize_t ofs = offsetof(SHArrayStorageSmall, storage) -
        HERMESVM_DIRECT_PROPERTY_SLOTS * sizeof(SHGCSmallHermesValue);
    if constexpr (ofs < 0)
      a.sub(xTemp1, xTemp1, -ofs);
    else
      a.add(xTemp1, xTemp1, ofs);
    emit_load_shv(
        a,
        hwRes.a64GpX(),
        a64::Mem(
            xTemp1, xTemp4, a64::Shift(a64::ShiftOp::kLSL, kPropShiftAmt)));
    shvDecode.emitAll(a);
    a.b(contLab);
  }

  /// Emit the chain. Entry state: xTemp1 = object, xTemp2 = compressed
  /// receiver class. Own entries, then chain entries (emitChainEntry),
  /// then the prototype block. At every own and chain entry's `next`
  /// label and at the fallthrough to the generic tier: xTemp1 = object,
  /// xTemp2 = compressed receiver class. Inside the prototype block,
  /// xTemp2 holds the parent's lazy ID at each `next` label instead; the
  /// code after its `join` label reloads the receiver class into xTemp2.
  /// hwRes may alias a temp; it is written only on hit paths, which
  /// branch to contLab. ID compares use xScratch for IDs that are not
  /// encodable as a compare immediate.
  void emitPolyChain(Emit_sh_shv_decode &shvDecode) {
    _.comment("// Get polymorphic chain (%u entries)", (unsigned)poly_.size());
    // xTemp3 = receiver lazyJITId, decoded without clobbering xTemp2.
    // Loaded even if every entry is a prototype entry.
    const auto &xReg =
        emit_sh_cp_decode_non_null_preserve_input(a, xTemp3, xTemp2);
    a.ldrh(xTemp3.w(), a64::Mem(xReg, RuntimeOffsets::hiddenClassLazyJITId));
    bool anyProto = false;
    for (const JitByIdPolyEntry &p : poly_) {
      if (p.kind == JitByIdEntryKind::Proto)
        anyProto = true;
      if (p.kind != JitByIdEntryKind::Own)
        continue;
      asmjit::Label next = a.newLabel();
      emit_cmp_imm32(a, xTemp3.w(), p.objId, xScratch.w());
      a.b_ne(next);
      // May clobber xTemp1 (indirect slot) and xTemp4; this path ends
      // at contLab.
      emitLoadFromSlot(hwRes.a64GpX(), xTemp1, xTemp4, p.slot);
      shvDecode.emitFirstCase(a);
      a.b(contLab);
      a.bind(next);
    }
    for (const JitByIdPolyEntry &p : poly_) {
      if (p.kind == JitByIdEntryKind::Chain)
        emitChainEntry(shvDecode, p);
    }
    if (!anyProto)
      return;
    asmjit::Label join = a.newLabel();
    // xTemp4 = parent, read from the receiver's class (xTemp2 still holds
    // it, compressed); xTemp2 = parent's lazyJITId (scratch from here on).
    const auto &xCls =
        emit_sh_cp_decode_non_null_preserve_input(a, xTemp4, xTemp2);
    emit_load_cp(
        a, xTemp4, a64::Mem(xCls, RuntimeOffsets::hiddenClassObjectParent));
    a.cbz(xTemp4, join);
    emit_sh_cp_decode_non_null(a, xTemp4);
    emit_load_cp(a, xTemp2, a64::Mem(xTemp4, offsetof(SHJSObject, clazz)));
    emit_sh_cp_decode_non_null(a, xTemp2);
    a.ldrh(xTemp2.w(), a64::Mem(xTemp2, RuntimeOffsets::hiddenClassLazyJITId));
    for (const JitByIdPolyEntry &p : poly_) {
      if (p.kind != JitByIdEntryKind::Proto)
        continue;
      asmjit::Label next = a.newLabel();
      emit_cmp_imm32(a, xTemp3.w(), p.objId, xScratch.w());
      a.b_ne(next);
      emit_cmp_imm32(a, xTemp2.w(), p.protoId, xScratch.w());
      a.b_ne(next);
      // Clobbers xTemp4/xTemp2 only after both guards succeeded.
      emitLoadFromSlot(hwRes.a64GpX(), xTemp4, xTemp2, p.slot);
      shvDecode.emitFirstCase(a);
      a.b(contLab);
      a.bind(next);
    }
    a.bind(join);
    // Restore the generic tier's operand.
    emit_load_cp(a, xTemp2, a64::Mem(xTemp1, offsetof(SHJSObject, clazz)));
  }

  /// Emit one chain entry (spec: "Emission", arm64). Entry state: xTemp1
  /// = object, xTemp2 = compressed receiver class, xTemp3 = receiver
  /// lazyJITId. A miss on either guard leaves all three untouched and
  /// clobbers only xTemp4, x16, x17 and NZCV. Past both guards the path
  /// counts the hit, follows the live chain p.depth levels from the
  /// receiver class's objectParent, loads the slot into hwRes (xTemp2 as
  /// the large-offset temporary) and ends at contLab.
  void emitChainEntry(
      Emit_sh_shv_decode &shvDecode,
      const JitByIdPolyEntry &p) {
    asmjit::Label next = a.newLabel();
    emit_cmp_imm32(a, xTemp3.w(), p.objId, xScratch.w());
    a.b_ne(next);
    // x17 = &entry.epoch; x16 = the runtime's epoch (the static_assert
    // above GetByIdImpl keeps its offset LDR-encodable).
    _.loadBits64InGp(xScratch2, (uint64_t)p.epochAddr, "chain entry epoch");
    a.ldr(xTemp4, a64::Mem(xScratch2));
    a.ldr(
        xScratch, a64::Mem(xRuntime, RuntimeOffsets::runtimeParentCacheEpoch));
    a.cmp(xTemp4, xScratch);
    a.b_ne(next);
    // Both guards passed; this path ends at contLab.
    _.emitIncrementCounter(JitCounter::NumByIdChainHits);
    // xTemp4 = the receiver's prototype, read from its class.
    const auto &xCls =
        emit_sh_cp_decode_non_null_preserve_input(a, xTemp4, xTemp2);
    emit_load_cp(
        a, xTemp4, a64::Mem(xCls, RuntimeOffsets::hiddenClassObjectParent));
    emit_sh_cp_decode_non_null(a, xTemp4);
    // Two dependent loads per further level: the class, then its parent.
    // No null checks: the receiver class and the epoch fix every level.
    for (unsigned i = 1; i < p.depth; ++i) {
      emit_load_cp(a, xTemp4, a64::Mem(xTemp4, offsetof(SHJSObject, clazz)));
      emit_sh_cp_decode_non_null(a, xTemp4);
      emit_load_cp(
          a, xTemp4, a64::Mem(xTemp4, RuntimeOffsets::hiddenClassObjectParent));
      emit_sh_cp_decode_non_null(a, xTemp4);
    }
    // Clobbers xTemp4 (indirect slot) and xTemp2 (large offset).
    emitLoadFromSlot(hwRes.a64GpX(), xTemp4, xTemp2, p.slot);
    shvDecode.emitFirstCase(a);
    a.b(contLab);
    a.bind(next);
  }

  /// Emit a specialization for accessing a property on the object.
  /// \return true if code was emitted. When it returns false nothing has
  ///   been emitted.
  bool emitObjectSpecialization(
      Emit_sh_shv_decode &shvDecode,
      ReadPropertyCacheEntry *cacheEntry) {
    // Obtain the HC ID.
    auto clazzID = _.initHCLazyIDMayAlloc(
        cacheEntry->clazz.get(_.runtime_, _.runtime_.getHeap()));
    if (!clazzID)
      return false;

    _.comment("// Get from object specialization");

    asmjit::Label failSpecLab = a.newLabel();

    // Decode the HC compressed pointer.
    const auto &xReg =
        emit_sh_cp_decode_non_null_preserve_input(a, xTemp3, xTemp2);
    // xTemp3 = hc->lazyJITId
    a.ldrh(xTemp3.w(), a64::Mem(xReg, RuntimeOffsets::hiddenClassLazyJITId));

    emit_cmp_imm32(a, xTemp3.w(), clazzID, xTemp4.w());
    a.b_ne(failSpecLab);
    // A match. Just load the property directly.
    emitLoadFromSlot(hwRes.a64GpX(), xTemp1, xTemp4, cacheEntry->getSlot());
    shvDecode.emitFirstCase(a);
    a.b(contLab);
    a.bind(failSpecLab);
    if (site_) {
      addByIdShape(
          *site_,
          JitByIdShapeEntry::makeOwn(
              cacheEntry->clazz.getNoBarrierUnsafe(),
              cacheEntry->getSlot(),
              true));
    }
    return true;
  }

  /// Emit a specialization for accessing a property on the parent.
  /// Does not preserve temps. Assumes no fast path runs after it.
  /// \return true if code was emitted. When it returns false nothing has been
  ///   emitted, so the caller is free to emit another tier instead.
  bool emitParentSpecialization(
      Emit_sh_shv_decode &shvDecode,
      ReadPropertyCacheEntry *cacheEntry) {
    // Obtain the HC IDs for the object's class and the parent class.
    // NOTE: every bail-out below must happen before the first instruction is
    // emitted, so that returning false leaves the caller a clean slate.
    auto clazzID = _.initHCLazyIDMayAlloc(
        cacheEntry->negMatchClazz.get(_.runtime_, _.runtime_.getHeap()));
    if (!clazzID)
      return false;

    // NOTE: the call above is a GC safepoint (it may create or grow the
    // usedHCs ArrayStorage), so cacheEntry->clazz, a WeakRoot, may have been
    // cleared in the meantime. initHCLazyIDMayAlloc tolerates a null class,
    // so this re-read is not load-bearing on its own; it is here to keep the
    // safepoint visible at the point where it matters, since nothing else in
    // this function suggests the previous line can collect.
    HiddenClass *parentCls =
        cacheEntry->clazz.get(_.runtime_, _.runtime_.getHeap());
    auto parentClsID = _.initHCLazyIDMayAlloc(parentCls);
    if (!parentClsID)
      return false;

    _.comment("// Get from parent specialization");

    asmjit::Label failSpecLab = a.newLabel();

    emit_sh_cp_decode_non_null(a, xTemp2);
    // xTemp2 = hc->lazyJITId
    a.ldrh(xTemp2.w(), a64::Mem(xTemp2, RuntimeOffsets::hiddenClassLazyJITId));
    // if object class mismatch, fail.
    emit_cmp_imm32(a, xTemp2.w(), clazzID, xTemp4.w());
    a.b_ne(failSpecLab);

    // Get the parent.
    emit_load_cp(a, xTemp1, a64::Mem(xTemp1, offsetof(SHJSObject, clazz)));
    emit_sh_cp_decode_non_null(a, xTemp1);
    emit_load_cp(
        a, xTemp1, a64::Mem(xTemp1, RuntimeOffsets::hiddenClassObjectParent));
    // If no parent, fail.
    a.cbz(xTemp1, failSpecLab);
    emit_sh_cp_decode_non_null(a, xTemp1);
    // Get the parent's hidden class.
    emit_load_cp(a, xTemp2, a64::Mem(xTemp1, offsetof(SHJSObject, clazz)));
    emit_sh_cp_decode_non_null(a, xTemp2);
    // xTemp2 = hc->lazyJITId
    a.ldrh(xTemp2.w(), a64::Mem(xTemp2, RuntimeOffsets::hiddenClassLazyJITId));
    // if parent class mismatch, fail.
    emit_cmp_imm32(a, xTemp2.w(), parentClsID, xTemp4.w());
    a.b_ne(failSpecLab);
    // A match. Just load the property directly.
    emitLoadFromSlot(hwRes.a64GpX(), xTemp1, xTemp4, cacheEntry->getSlot());
    shvDecode.emitAll(a);
    a.b(contLab);
    a.bind(failSpecLab);
    if (site_) {
      addByIdShape(
          *site_,
          JitByIdShapeEntry::makeProto(
              cacheEntry->negMatchClazz.getNoBarrierUnsafe(),
              cacheEntry->clazz.getNoBarrierUnsafe(),
              cacheEntry->getSlot(),
              true));
    }
    return true;
  }
};

void Emitter::getByIdImpl(
    FR frRes,
    SHSymbolID symID,
    FR frSource,
    uint8_t cacheIdx,
    const char *name,
    SHLegacyValue (*shImpl)(
        SHRuntime *shr,
        const SHLegacyValue *source,
        SHSymbolID symID,
        SHReadPropertyCacheEntry *propCacheEntry),
    const char *shImplName,
    bool recording) {
  GetByIdImpl(
      *this,
      frRes,
      symID,
      frSource,
      cacheIdx,
      name,
      shImpl,
      shImplName,
      recording)
      .run();
}

void Emitter::getByIdWithReceiver(
    FR frRes,
    SHSymbolID symID,
    FR frSource,
    FR frReceiver,
    uint8_t cacheIdx) {
  comment(
      "// GetByIdWithReceiver r%u, r%u, r%u, cache %u, symID %u",
      frRes.index(),
      frSource.index(),
      frReceiver.index(),
      cacheIdx,
      symID);

  // TODO: Add a fast path, probably by sharing code with getByIdImpl.

  syncAllFRTempExcept(frRes != frSource && frRes != frReceiver ? frRes : FR());
  syncToFrame(frSource);
  syncToFrame(frReceiver);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frSource);
  loadFrameAddr(a64::x2, frReceiver);
  a.mov(a64::w3, symID);
  if (cacheIdx == hbc::PROPERTY_CACHING_DISABLED) {
    a.mov(a64::x4, 0);
  } else {
    a.ldr(a64::x4, a64::Mem(roDataLabel_, roOfsReadPropertyCachePtr_));
    if (cacheIdx != 0)
      emit_add_imm_u24(a, a64::x4, sizeof(SHReadPropertyCacheEntry) * cacheIdx);
  }
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(
          SHRuntime *shr,
          const SHLegacyValue *source,
          const SHLegacyValue *receiver,
          SHSymbolID symID,
          SHReadPropertyCacheEntry *propCacheEntry),
      _sh_ljs_get_by_id_with_receiver_rjs);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::getByValWithReceiver(
    FR frRes,
    FR frSource,
    FR frKey,
    FR frReceiver) {
  comment(
      "// GetByValWithReceiver r%u, r%u, r%u, r%u",
      frRes.index(),
      frSource.index(),
      frReceiver.index(),
      frKey.index());

  syncAllFRTempExcept(
      frRes != frSource && frRes != frReceiver && frRes != frKey ? frRes
                                                                 : FR());
  syncToFrame(frSource);
  syncToFrame(frKey);
  syncToFrame(frReceiver);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frSource);
  loadFrameAddr(a64::x2, frKey);
  loadFrameAddr(a64::x3, frReceiver);

  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(
          SHRuntime *shr,
          SHLegacyValue *source,
          SHLegacyValue *key,
          SHLegacyValue *receiver),
      _sh_ljs_get_by_val_with_receiver_rjs);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::emitGetByValFastArrayTier(
    const GetByValRegs &regs,
    const asmjit::Label &helperLab,
    bool sourceKnownObject) {
  comment("// Inline fast array load");

  // Every register this runs on was chosen by getByValImpl(); see
  // GetByValRegs. Nothing here allocates or frees, so this emits code and
  // nothing else.
  static_assert(
      HERMESVALUE_VERSION == 2,
      "non-numbers must be NaN-encoded for the key test below");
  const a64::GpX &xSource = regs.source;
  const a64::VecD &dKey = regs.key;
  // Holds the object, then the indexed storage, then the element address.
  const a64::GpX &xLoc = regs.loc;
  const a64::GpX &xIdx = regs.idx;
  const a64::GpX &xTemp1 = regs.temp1;
  const a64::VecD &dKeyTmp = regs.keyTmp;
  const a64::GpX &xRes = regs.res;
  assert(dKeyTmp != dKey && "emit_double_is_uint32() needs a distinct temp");

  // Is the source an object? At a site with a typed-array tier ahead of
  // this one, that tier has already proved it, and nothing runs between
  // the two tiers that could change the answer.
  if (!sourceKnownObject) {
    emit_sh_ljs_is_object(a, xTemp1, xSource);
    a.b_ne(helperLab);
  }
  // xLoc is the pointer to the object.
  emit_sh_ljs_get_pointer(a, xLoc, xSource);

  // Is it a JSArray, and nothing else? Exactly the put tier's guard: the
  // exact CellKind is what lets this code skip ObjectVTable dispatch, and
  // restricting to JSArray (rather than all of ArrayImpl) is what lets an
  // Arguments object decline to the helper instead of being read inline.
  emit_gccell_get_kind(a, xTemp1, xLoc);
  a.cmp(xTemp1.w(), (uint32_t)CellKind::JSArrayKind);
  a.b_ne(helperLab);

  // Unlike emitPutByValFastArrayTier(), no fastIndexProperties/frozen check
  // here: tryFastGetComputedNoAlloc()'s own comment (JSObject-inline.h)
  // explains why one is not needed on the read side at all -- "We don't
  // need to check for fast index properties here, because if there are
  // non-fast ones, the corresponding slot will be empty" -- so the empty
  // check below already subsumes it.

  // toArrayIndexFastPath(): the key must be a double that survives a round
  // trip through uint32, and must not be 0xFFFFFFFF, which JS arrays do not
  // accept as an index. Reused verbatim from emitPutByValFastArrayTier(),
  // including its single exit for a NaN key: an unordered fcmp leaves Z
  // clear, so the b.ne takes it.
  emit_double_is_uint32(a, xIdx.w(), dKeyTmp, dKey);
  a.b_ne(helperLab);
  // idx == 0xFFFFFFFF, as one instruction: cmn adds, so Z is set exactly when
  // idx + 1 wraps to zero.
  a.cmn(xIdx.w(), 1);
  a.b_eq(helperLab);

  // ArrayImpl::at()'s range test: `index - beginIndex_ < elemCount_`,
  // unsigned, one comparison for both ends -- identical to
  // _haveOwnIndexedImpl()'s/_setOwnIndexedImpl()'s, which the put tier also
  // relies on. xIdx becomes the index into the storage, and the 32-bit
  // subtract zeroes its upper half, which is what makes the scaled 64-bit
  // index below the uint32 one. Out of range is a DECLINE, not `undefined`:
  // the prototype chain may carry an indexed property at that index, so only
  // the full path can answer.
  static_assert(
      RuntimeOffsets::arrayImplBeginIndex % 4 == 0 &&
          RuntimeOffsets::arrayImplElemCount % 4 == 0 &&
          RuntimeOffsets::arrayImplBeginIndex < maxNaturalBaseOffset(4) &&
          RuntimeOffsets::arrayImplElemCount < maxNaturalBaseOffset(4),
      "the array range fields must be reachable by unsigned-offset LDRs");
  a.ldr(xTemp1.w(), a64::Mem(xLoc, RuntimeOffsets::arrayImplBeginIndex));
  a.sub(xIdx.w(), xIdx.w(), xTemp1.w());
  a.ldr(xTemp1.w(), a64::Mem(xLoc, RuntimeOffsets::arrayImplElemCount));
  a.cmp(xIdx.w(), xTemp1.w());
  a.b_hs(helperLab);

  // The storage. It cannot be null here: elemCount_ is 0 whenever
  // indexedStorage_ is, which the comparison above has already ruled out --
  // the same reasoning emitPutByValFastArrayTier() relies on.
  static_assert(
      RuntimeOffsets::arrayImplIndexedStorage % sizeof(CompressedPointer) ==
              0 &&
          RuntimeOffsets::arrayImplIndexedStorage <
              maxNaturalBaseOffset(sizeof(CompressedPointer)),
      "the indexed storage must be reachable by an unsigned-offset LDR");
  emit_load_cp(
      a, xLoc, a64::Mem(xLoc, RuntimeOffsets::arrayImplIndexedStorage));
  emit_sh_cp_decode_non_null(a, xLoc);

  // The address of the element. The scale is the width of a heap value slot,
  // which is four bytes under compressed pointers and eight otherwise -- an
  // element index is not a byte offset in any mode. Unlike
  // emitPutByValFastArrayTier() there is no jumbo-cell size gate here: that
  // guard exists solely for emitSafeStoreOrSlow()'s write-barrier card math,
  // which a load never performs.
  a.add(xLoc, xLoc, xIdx, a64::lsl(RuntimeOffsets::kLogSmallHermesValueSize));
  static_assert(
      offsetof(SHArrayStorageSmall, storage) <= 0xFFFFFF,
      "the element base offset must fit emit_add_imm_u24()");
  emit_add_imm_u24(a, xLoc, offsetof(SHArrayStorageSmall, storage));

  // ArrayImpl::at() reports `empty` for a hole -- a read at a hole is not a
  // fast-path read at all, exactly as a write to one is not a fast-path
  // write: the runtime resolves the property normally, and may find a data
  // property or an accessor on the prototype chain. Decline, and let the
  // helper do that.
  //
  // Unlike the put tier's use of emit_shv_load_is_empty(), this loads the
  // raw bits into xRes itself, because the decode below needs to consume
  // them on the non-empty path. That rules out reusing
  // emit_shv_load_is_empty() outright: in its 8-byte-slot branch it runs the
  // check via emit_sh_ljs_is_empty(a, xTempReg, xTempReg), which is
  // documented to modify its input when the check and temp registers are the
  // same -- exactly the case here, and it would leave xRes holding the
  // shifted ETag instead of the value to decode. The 4-byte-slot branch has
  // no such hazard (cmp does not write its operand), so only the 8-byte case
  // needs its own temp for the check.
  assert(xTemp1 != xRes && "the empty check needs a temp distinct from xRes");
  emit_load_shv(a, xRes, a64::Mem(xLoc));
  if constexpr (sizeof(SmallHermesValue) == 4) {
    emit_cmp_imm32(
        a,
        xRes.w(),
        (uint32_t)SmallHermesValue::encodeEmptyValue().getRaw(),
        xScratch.w());
  } else {
    emit_sh_ljs_is_empty(a, xTemp1, xRes);
  }
  a.b_eq(helperLab);

  // Unbox the SmallHermesValue into the HermesValue the result register must
  // hold. HV64: identity, nothing is emitted. HV32/BOXED: the same total
  // (never-declining) small-value/compressed-pointer/boxed-double decode
  // getOwnBySlotIdx() uses for a property slot's SmallHermesValue -- this is
  // the load side's mirror of emit_shv_encode_for_slot_or_slow(), and unlike
  // that encode it never has anything to decline: every SmallHermesValue
  // this element slot can hold decodes to some HermesValue.
  asmjit::Label doneLab = a.newLabel();
  emit_sh_shv_decode(a, xRes, doneLab);
  a.bind(doneLab);
}

/// The element width of typed-array \p kind, as a log2 byte count. On arm64
/// this doubles as the scale an element access's addressing mode must carry:
/// both natural forms -- a register index extended by `uxtw #logWidth`, and a
/// 12-bit unsigned immediate -- are scaled by the access size, so a scaled
/// element access IS the addressing mode here, with no separate shifted add
/// (unlike the fast array tiers' slot arithmetic).
///
/// One definition for all three users -- emitTypedArrayElementLoad()'s
/// cross-check assert, and the two typed-array load tiers that build the
/// address it checks -- so that a width can never disagree between the
/// address and the load made through it.
static uint32_t typedArrayLogWidth(CellKind kind) {
  switch (kind) {
    case CellKind::Uint8ArrayKind:
    case CellKind::Uint8ClampedArrayKind:
    case CellKind::Int8ArrayKind:
      return 0;
    case CellKind::Uint16ArrayKind:
    case CellKind::Int16ArrayKind:
      return 1;
    case CellKind::Uint32ArrayKind:
    case CellKind::Int32ArrayKind:
    case CellKind::Float32ArrayKind:
      return 2;
    case CellKind::Float64ArrayKind:
      return 3;
    default:
      llvm_unreachable("unsupported typed-array load tier kind");
  }
}

void Emitter::emitTypedArrayElementLoad(
    CellKind kind,
    const a64::Mem &elemBase,
    const a64::GpX &xRes,
    const a64::GpX &xTemp1,
    const a64::VecD &dValTmp,
    const asmjit::Label &doneLab) {
  const bool isFloat64 = kind == CellKind::Float64ArrayKind;
  const bool isFloat32 = kind == CellKind::Float32ArrayKind;
  const uint32_t logWidth = typedArrayLogWidth(kind);
  (void)logWidth;
  // Whichever of the two addressing forms the caller built, it has to be one
  // this width can encode: a register index is scaled by the access size, so
  // its shift must BE the element's log width, and an immediate offset is
  // scaled the same way, so it must be a multiple of the width and inside
  // the 12-bit scaled range. (An immediate that is not would silently become
  // an unscaled LDUR, whose reach is 9 signed bits.)
  //
  // The index form's EXTEND is checked too, not just its shift: the index is
  // a 32-bit element index in the low half of a 64-bit register, so the only
  // correct extend is `uxtw`. An `lsl`-built Mem carries the right shift and
  // would pass a shift-only assert, but it encodes the index as a full
  // 64-bit register (`uxtx`), reading whatever the upper half happens to
  // hold.
  assert(
      (elemBase.hasIndex()
           ? elemBase.shift() == logWidth &&
               elemBase.shiftOp() == a64::ShiftOp::kUXTW
           : (uint32_t)elemBase.offsetLo32() % (1u << logWidth) == 0 &&
               (uint32_t)elemBase.offsetLo32() <
                   maxNaturalBaseOffset(1u << logWidth)) &&
      "the element address must encode at this kind's width");

  // Float32 loads through the single-precision view of the same vector
  // register the widened double lands in.
  const a64::VecS sValTmp{dValTmp.id()};

  // The element load and its widening to a double.
  //
  // Every integer kind loads into a 32-bit GP register and converts from
  // there, which is exact for all of them: the signed kinds sign-extend and
  // use the signed convert, the small unsigned kinds zero-extend into a
  // value the signed convert also represents exactly, and Uint32 -- the one
  // kind whose values can exceed INT32_MAX -- uses arm64's UNSIGNED convert
  // directly, so it needs no 64-bit detour (x86-64, which has no unsigned
  // convert, does). Uint8Clamped reads exactly like Uint8: clamping is a
  // store-side conversion and leaves no trace in the stored byte.
  switch (kind) {
    case CellKind::Int8ArrayKind:
      a.ldrsb(xTemp1.w(), elemBase);
      a.scvtf(dValTmp, xTemp1.w());
      break;
    case CellKind::Uint8ArrayKind:
    case CellKind::Uint8ClampedArrayKind:
      a.ldrb(xTemp1.w(), elemBase);
      a.scvtf(dValTmp, xTemp1.w());
      break;
    case CellKind::Int16ArrayKind:
      a.ldrsh(xTemp1.w(), elemBase);
      a.scvtf(dValTmp, xTemp1.w());
      break;
    case CellKind::Uint16ArrayKind:
      a.ldrh(xTemp1.w(), elemBase);
      a.scvtf(dValTmp, xTemp1.w());
      break;
    case CellKind::Int32ArrayKind:
      a.ldr(xTemp1.w(), elemBase);
      a.scvtf(dValTmp, xTemp1.w());
      break;
    case CellKind::Uint32ArrayKind:
      a.ldr(xTemp1.w(), elemBase);
      a.ucvtf(dValTmp, xTemp1.w());
      break;
    case CellKind::Float32ArrayKind:
      a.ldr(sValTmp, elemBase);
      a.fcvt(dValTmp, sValTmp);
      break;
    case CellKind::Float64ArrayKind:
      a.ldr(dValTmp, elemBase);
      break;
    default:
      llvm_unreachable("unsupported typed-array load tier kind");
  }

  // NaN CANONICALIZATION, float kinds only. A float element holds whatever
  // 32 or 64 bits were written into it, and every NaN payload is a legal
  // element value; the tag space this engine encodes non-numbers in lives
  // inside the NaN space, so moving such an element's bits into the result
  // verbatim would forge a pointer, a bool or a symbol out of a number.
  // encodeUntrustedNumberValue() is what the interpreter's own read applies
  // here, and this is its inline form: self-compare, and on unordered (which
  // no other value gives) replace the whole pattern with the canonical quiet
  // NaN. The integer kinds cannot produce a NaN at all -- a convert of any
  // integer is finite -- so they skip it, as encodeTrustedNumberValue() does.
  if (isFloat32 || isFloat64) {
    asmjit::Label encodeLab = a.newLabel();
    a.fcmp(dValTmp, dValTmp);
    a.b_vc(encodeLab);
    loadBits64InGp(
        xTemp1,
        (uint64_t)HermesValue::encodeNaNValue().getRaw(),
        "canonical NaN");
    a.fmov(dValTmp, xTemp1);
    a.bind(encodeLab);
  }

  // Encode the double as a number HermesValue, which under NaN-boxing is its
  // raw bit pattern -- in every heap-value mode, since the mode changes only
  // how a value is stored in the HEAP, never how it is held in a frame.
  a.fmov(xRes, dValTmp);
  a.b(doneLab);
}

void Emitter::emitGetByValTypedArrayTier(
    const GetByValRegs &regs,
    CellKind kind,
    const asmjit::Label &kindMissLab,
    const asmjit::Label &helperLab) {
  comment("// Inline typed array load (kind %u)", (unsigned)kind);

  // Every register this runs on was chosen by getByValImpl(); see
  // GetByValRegs. Nothing here allocates or frees.
  static_assert(
      HERMESVALUE_VERSION == 2,
      "non-numbers must be NaN-encoded for the key test below");
  const a64::GpX &xSource = regs.source;
  const a64::VecD &dKey = regs.key;
  // Holds the object, then the buffer, then the element base address.
  const a64::GpX &xLoc = regs.loc;
  const a64::GpX &xIdx = regs.idx;
  const a64::GpX &xTemp1 = regs.temp1;
  const a64::VecD &dKeyTmp = regs.keyTmp;
  // Holds the loaded element, widened to the double it must become.
  const a64::VecD &dValTmp = regs.valTmp;
  const a64::GpX &xRes = regs.res;
  assert(dKeyTmp != dKey && "emit_double_is_uint32() needs a distinct temp");

  // The element width, as a log2 byte count: the scale of the load's index
  // operand, since the index is an element count.
  const uint32_t logWidth = typedArrayLogWidth(kind);

  // The object and exact-kind checks come FIRST: at a duo site a kind miss
  // must reach the JSArray tier, and a non-object cannot be read by either
  // tier, so only the kind check chains.
  emit_sh_ljs_is_object(a, xTemp1, xSource);
  a.b_ne(helperLab);
  emit_sh_ljs_get_pointer(a, xLoc, xSource);
  emit_gccell_get_kind(a, xTemp1, xLoc);
  a.cmp(xTemp1.w(), (uint32_t)kind);
  a.b_ne(kindMissLab);

  // No object-flags check, unlike emitPutByValTypedArrayTier(): the read path
  // checks neither fastIndexProperties nor frozen for a typed array
  // (tryFastGetComputedNoAlloc(), JSObject-inline.h), because an element read
  // is answered by length and attachedness alone. Checking them would decline
  // reads the interpreter answers inline.

  // The key must be a double that converts to a uint32 and back unchanged.
  // As in the fast array tier one exit covers every rejection, a NaN key
  // included. Unlike JS arrays, typed arrays need no 0xFFFFFFFF exclusion:
  // the bounds check below rejects it.
  emit_double_is_uint32(a, xIdx.w(), dKeyTmp, dKey);
  a.b_ne(helperLab);

  // From here on the answer is this tier's, whatever it is: an out-of-bounds
  // index and a detached buffer both read as `undefined`
  // (tryFastGetComputedNoAlloc()'s typed-array branch returns exactly that),
  // so they branch to a local `undefined` rather than to the helper. Nothing
  // is recorded on that path, and nothing should be: the source's shape there
  // IS the specialized kind.
  asmjit::Label undefLab = a.newLabel();
  asmjit::Label doneLab = a.newLabel();

  // Bounds: idx < length_, one unsigned 32-bit compare. This tree has no
  // resizable ArrayBuffers, so length_ is fixed for the object's lifetime.
  static_assert(
      RuntimeOffsets::jsTypedArrayBaseLength % 4 == 0 &&
          RuntimeOffsets::jsTypedArrayBaseLength < maxNaturalBaseOffset(4),
      "the typed array length must be reachable by an unsigned-offset LDR");
  a.ldr(xTemp1.w(), a64::Mem(xLoc, RuntimeOffsets::jsTypedArrayBaseLength));
  a.cmp(xIdx.w(), xTemp1.w());
  a.b_hs(undefLab);

  // The element base address: the buffer's data_, plus the view's byte
  // offset_ into it. A detached buffer has a null data_, so the attached
  // check is free with a load the read needs anyway. The offset goes in a
  // real temp, NOT in xScratch: on arm64 xScratch is what mask and immediate
  // materializations use, so nothing may live there across other emitters.
  // The 32-bit load zeroes the upper half of xTemp1, which is what makes the
  // 64-bit add below the zero-extended one. Copied from
  // emitPutByValTypedArrayTier(), whose address computation is the same one.
  static_assert(
      RuntimeOffsets::jsTypedArrayBaseOffset % 4 == 0 &&
          RuntimeOffsets::jsTypedArrayBaseOffset < maxNaturalBaseOffset(4),
      "the typed array offset must be reachable by an unsigned-offset LDR");
  static_assert(
      RuntimeOffsets::jsTypedArrayBaseBuffer % sizeof(CompressedPointer) == 0 &&
          RuntimeOffsets::jsTypedArrayBaseBuffer <
              maxNaturalBaseOffset(sizeof(CompressedPointer)),
      "the typed array buffer must be reachable by an unsigned-offset LDR");
  static_assert(
      RuntimeOffsets::jsArrayBufferData % 8 == 0 &&
          RuntimeOffsets::jsArrayBufferData < maxNaturalBaseOffset(8),
      "the buffer data pointer must be reachable by an unsigned-offset LDR");
  a.ldr(xTemp1.w(), a64::Mem(xLoc, RuntimeOffsets::jsTypedArrayBaseOffset));
  emit_load_cp(a, xLoc, a64::Mem(xLoc, RuntimeOffsets::jsTypedArrayBaseBuffer));
  emit_sh_cp_decode_non_null(a, xLoc);
  a.ldr(xLoc, a64::Mem(xLoc, RuntimeOffsets::jsArrayBufferData));
  a.cbz(xLoc, undefLab);
  a.add(xLoc, xLoc, xTemp1);

  // The element access itself. xIdx is an UNSCALED element index, and
  // emit_double_is_uint32() left its upper 32 bits zero, so the shared tail
  // reaches the element through the UXTW index form, which applies this
  // kind's scale itself.
  emitTypedArrayElementLoad(
      kind,
      a64::Mem(xLoc, xIdx.w(), a64::uxtw(logWidth)),
      xRes,
      xTemp1,
      dValTmp,
      doneLab);

  a.bind(undefLab);
  loadBits64InGp(xRes, (uint64_t)_sh_ljs_undefined().raw, "undefined");
  a.bind(doneLab);
}

void Emitter::getByValImpl(FR frRes, FR frSource, FR frKey) {
  // The GetByVal instruction's own bytecode offset, unique within this
  // function and stable across recompiles: identifies this site's entry in
  // versionData_'s consumer records (see JitFunctionData.h).
  uint32_t siteId =
      (uint32_t)((const char *)emittingIP - (const char *)codeBlock_->begin());

  comment(
      "// getByVal r%u, r%u, r%u",
      frRes.index(),
      frSource.index(),
      frKey.index());

  syncAllFRTempExcept(frRes != frSource && frRes != frKey ? frRes : FR());
  syncToFrame(frSource);
  syncToFrame(frKey);

  // Monotone tier selection, exactly putByValImpl()'s -- see its comment for
  // why the JSArray tier is a static prior rather than a decision, and why
  // selecting the typed-array tier from the observed field alone is already
  // monotone. The JSArray LOAD tier additionally needs no
  // HERMES_JIT_INLINE_SAFE_STORE gate: a load takes no write barrier, so it
  // is emitted under every GC, MallocGC included.
  //
  // The predicate here is the LOAD one, which admits Uint8Clamped where the
  // store predicate does not; the recording helper for a get site validated
  // the kind against this same predicate before storing it.
  JitByValSiteRecord &site = byValSiteRecord(siteId);
  const bool emitTA = site.taKind != JitByValSiteRecord::kTAKindNone &&
      isJitSupportedTypedArrayLoadKind((CellKind)site.taKind);
  site.specializedTAKind =
      emitTA ? site.taKind : JitByValSiteRecord::kTAKindNone;

  asmjit::Label helperLab = a.newLabel();
  asmjit::Label contLab = a.newLabel();

  // The register assignment for the whole tier chain, made once (see
  // GetByValRegs). This is the only register-allocator bookkeeping in the
  // sequence: the tiers themselves only emit code, and after this block the
  // allocator is already in the state the shared helper call needs -- every
  // temp free, and no FR but frRes registered in one.
  //
  // The allocs and frees below emit no code at all: they only decide which
  // registers the tiers may use. The operand loads do emit code, once, for
  // both tiers -- a tier that declines leaves the source and the key
  // untouched, so the next tier in the chain reads the same registers.
  freeAllFRTempExcept(frSource);
  // The source is needed as a raw HermesValue in a GP register. The key is
  // needed as the double it has to be, which is what emit_double_is_uint32()
  // consumes; getOrAllocFRInVecD() only moves the raw 64 bits -- fmov or ldr,
  // never a conversion -- so this is safe for a key of any type: a non-number
  // is NaN-encoded and every tier's key conversion rejects it.
  HWReg hwSource = getOrAllocFRInGpX(frSource, true);
  HWReg hwKey = getOrAllocFRInVecD(frKey, true);
  HWReg hwLoc = allocTempGpX();
  HWReg hwIdx = allocTempGpX();
  HWReg hwTemp1 = allocTempGpX();
  HWReg hwKeyTmp = allocTempVecD();
  HWReg hwValTmp = emitTA ? allocTempVecD() : HWReg{};
  freeReg(hwLoc);
  freeReg(hwIdx);
  freeReg(hwTemp1);
  freeReg(hwKeyTmp);
  if (emitTA)
    freeReg(hwValTmp);
  freeFRTemp(frSource);
  freeFRTemp(frKey);
  // The result register comes last, so it may overlap the temps just freed --
  // the same ordering GetByIdImpl::emitFastPath() uses for hwRes. The gpX(0)
  // hint is what lets the slow path (whose helper call returns in x0)
  // reconcile with this register at no cost in the common case. `false`: this
  // is a fresh output, no current value to load.
  HWReg hwRes = getOrAllocFRInGpX(frRes, false, HWReg::gpX(0));

  GetByValRegs regs;
  regs.source = hwSource.a64GpX();
  regs.key = hwKey.a64VecD();
  regs.loc = hwLoc.a64GpX();
  regs.idx = hwIdx.a64GpX();
  regs.temp1 = hwTemp1.a64GpX();
  regs.keyTmp = hwKeyTmp.a64VecD();
  if (emitTA)
    regs.valTmp = hwValTmp.a64VecD();
  regs.res = hwRes.a64GpX();
  // res may share a register with loc or idx -- both are dead by the time any
  // tier writes it -- but NOT with temp1, which the fast array tier still
  // needs after loading the element into res (see its empty check). The
  // allocation above guarantees it: res is either a global register, which is
  // never a temp, or x0, which is free here and therefore was handed out as
  // the FIRST temp, loc.
  assert(regs.temp1 != regs.res && "the result must not alias temp1");

  if (emitTA) {
    // The recorded kind is compared first: it is what the site was observed
    // declining on. Its kind miss chains into the JSArray tier below, never
    // into the helper.
    asmjit::Label kindMissLab = a.newLabel();
    emitGetByValTypedArrayTier(
        regs, (CellKind)site.taKind, kindMissLab, helperLab);
    // Falling out of the inline tier means the load is done.
    a.b(contLab);
    a.bind(kindMissLab);
  }
  emitGetByValFastArrayTier(regs, helperLab, /* sourceKnownObject */ emitTA);
  // Falling out of the inline tier means the load is done.
  a.b(contLab);
  a.bind(helperLab);

  // No freeAllFRTempExcept({}) here: the prologue above already left every
  // temp free and no FR but frRes registered in one, and frRes's register
  // must survive into the call below so the slow-path result lands in the
  // same register the tiers used -- the same structure GetByIdImpl::run()
  // uses between its fast path and its shared call.
  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frSource);
  loadFrameAddr(a64::x2, frKey);
  loadBits64InGp(a64::x3, (uint64_t)versionData_, "JitVersionData");
  a.mov(a64::w4, siteId);

  // site.helper is per-site mutable state (spec: "Slow-path demotion: the
  // pointer flip"): it starts out null on a fresh record and is carried
  // forward as-is across recompiles, so a demoted slot (flipped by the
  // runtime to the plain _sh_ljs_get_by_val_rjs) stays demoted. Only a
  // still-null slot -- never yet flipped -- is initialized here.
  if (!site.helper)
    site.helper = (void *)_jit_get_by_val;
  // Type-check the recording callee against the signature the emitted call
  // sequence assumes, the same way EMIT_RUNTIME_CALL does for a direct call
  // -- even though it is not called directly here, since the actual callee is
  // loaded from site.helper at runtime. The demoted callee takes three
  // arguments rather than five; under AAPCS64 the extra argument registers
  // are simply ignored, exactly as for the PutByVal sites' six-versus-four.
  using _FnT = SHLegacyValue (*)(
      SHRuntime *,
      SHLegacyValue *,
      SHLegacyValue *,
      SHJitVersionData *,
      uint32_t);
  _FnT _fn = _jit_get_by_val;
  (void)_fn;
  // WithSavedIP, not the plain indirect call: the fallback resolves the
  // property, which may run a getter and may throw.
  callRuntimeWithSavedIPIndirect(
      (uint64_t)&site.helper, "_jit_get_by_val [indirect]");

  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);

  a.bind(contLab);
}

void Emitter::getByVal(FR frRes, FR frSource, FR frKey) {
  getByValImpl(frRes, frSource, frKey);
}

void Emitter::emitGetByIndexFastArrayTier(
    const GetByIndexRegs &regs,
    uint32_t key,
    const asmjit::Label &helperLab,
    bool sourceKnownObject) {
  comment("// Inline fast array load");

  // Every register this runs on was chosen by getByIndexImpl(); see
  // GetByIndexRegs. Nothing here allocates or frees, so this emits code and
  // nothing else.
  const a64::GpX &xSource = regs.source;
  // Holds the object, then the indexed storage, then the element address.
  const a64::GpX &xLoc = regs.loc;
  const a64::GpX &xIdx = regs.idx;
  const a64::GpX &xTemp1 = regs.temp1;
  const a64::GpX &xRes = regs.res;

  // Is the source an object? At a site with a typed-array tier ahead of
  // this one, that tier has already proved it, and nothing runs between
  // the two tiers that could change the answer.
  if (!sourceKnownObject) {
    emit_sh_ljs_is_object(a, xTemp1, xSource);
    a.b_ne(helperLab);
  }
  // xLoc is the pointer to the object.
  emit_sh_ljs_get_pointer(a, xLoc, xSource);

  // Is it a JSArray, and nothing else? Exactly emitGetByValFastArrayTier()'s
  // guard: the exact CellKind is what lets this code skip ObjectVTable
  // dispatch, and restricting to JSArray (rather than all of ArrayImpl) is
  // what lets an Arguments object decline to the helper instead of being
  // read inline.
  emit_gccell_get_kind(a, xTemp1, xLoc);
  a.cmp(xTemp1.w(), (uint32_t)CellKind::JSArrayKind);
  a.b_ne(helperLab);

  // No fastIndexProperties/frozen check here, for the same reason
  // emitGetByValFastArrayTier() has none: the empty check below already
  // subsumes it.

  // ArrayImpl::at()'s range test, with K an emit-time constant:
  // `K - beginIndex_ < elemCount_`, unsigned, one comparison for both ends.
  // K being constant does NOT make the storage offset constant -- a first
  // indexed write can set beginIndex_ to any index (JSArray.cpp) -- so the
  // subtraction still has to happen at run time, and the constant has to be
  // materialized into a register to be the MINUEND of it. The 32-bit
  // mov/sub/cmp sequence zero-extends xIdx, which is what makes the scaled
  // 64-bit index below the uint32 one. Out of range is a DECLINE, not
  // `undefined`: the prototype chain may carry an indexed property at that
  // index, so only the full path can answer.
  static_assert(
      RuntimeOffsets::arrayImplBeginIndex % 4 == 0 &&
          RuntimeOffsets::arrayImplElemCount % 4 == 0 &&
          RuntimeOffsets::arrayImplBeginIndex < maxNaturalBaseOffset(4) &&
          RuntimeOffsets::arrayImplElemCount < maxNaturalBaseOffset(4),
      "the array range fields must be reachable by unsigned-offset LDRs");
  a.mov(xIdx.w(), key);
  a.ldr(xTemp1.w(), a64::Mem(xLoc, RuntimeOffsets::arrayImplBeginIndex));
  a.sub(xIdx.w(), xIdx.w(), xTemp1.w());
  a.ldr(xTemp1.w(), a64::Mem(xLoc, RuntimeOffsets::arrayImplElemCount));
  a.cmp(xIdx.w(), xTemp1.w());
  a.b_hs(helperLab);

  // The storage. It cannot be null here: elemCount_ is 0 whenever
  // indexedStorage_ is, which the comparison above has already ruled out.
  static_assert(
      RuntimeOffsets::arrayImplIndexedStorage % sizeof(CompressedPointer) ==
              0 &&
          RuntimeOffsets::arrayImplIndexedStorage <
              maxNaturalBaseOffset(sizeof(CompressedPointer)),
      "the indexed storage must be reachable by an unsigned-offset LDR");
  emit_load_cp(
      a, xLoc, a64::Mem(xLoc, RuntimeOffsets::arrayImplIndexedStorage));
  emit_sh_cp_decode_non_null(a, xLoc);

  // The address of the element. The scale is the width of a heap value slot,
  // which is four bytes under compressed pointers and eight otherwise -- an
  // element index is not a byte offset in any mode. Verbatim from
  // emitGetByValFastArrayTier(), including the absence of a jumbo-cell size
  // gate: that guard exists solely for emitSafeStoreOrSlow()'s write-barrier
  // card math, which a load never performs.
  a.add(xLoc, xLoc, xIdx, a64::lsl(RuntimeOffsets::kLogSmallHermesValueSize));
  static_assert(
      offsetof(SHArrayStorageSmall, storage) <= 0xFFFFFF,
      "the element base offset must fit emit_add_imm_u24()");
  emit_add_imm_u24(a, xLoc, offsetof(SHArrayStorageSmall, storage));

  // ArrayImpl::at() reports `empty` for a hole -- a read at a hole is not a
  // fast-path read at all: the runtime resolves the property normally, and
  // may find a data property or an accessor on the prototype chain. Decline,
  // and let the helper do that. This is emitGetByValFastArrayTier()'s hole
  // check verbatim, including why emit_shv_load_is_empty() cannot serve
  // here: the raw bits are loaded INTO xRes because the decode below has to
  // consume them, and that helper's 8-byte-slot branch shifts its input in
  // place, which would leave xRes holding the shifted ETag instead of the
  // value. Hence the check runs through a DISTINCT temp. The 4-byte-slot
  // branch has no such hazard (cmp does not write its operand).
  assert(xTemp1 != xRes && "the empty check needs a temp distinct from xRes");
  emit_load_shv(a, xRes, a64::Mem(xLoc));
  if constexpr (sizeof(SmallHermesValue) == 4) {
    emit_cmp_imm32(
        a,
        xRes.w(),
        (uint32_t)SmallHermesValue::encodeEmptyValue().getRaw(),
        xScratch.w());
  } else {
    emit_sh_ljs_is_empty(a, xTemp1, xRes);
  }
  a.b_eq(helperLab);

  // Unbox the SmallHermesValue into the HermesValue the result register must
  // hold -- the same shared decode emitGetByValFastArrayTier() uses, which
  // never has anything left to decline.
  asmjit::Label doneLab = a.newLabel();
  emit_sh_shv_decode(a, xRes, doneLab);
  a.bind(doneLab);
}

void Emitter::emitGetByIndexTypedArrayTier(
    const GetByIndexRegs &regs,
    uint32_t key,
    CellKind kind,
    const asmjit::Label &kindMissLab,
    const asmjit::Label &helperLab) {
  comment("// Inline typed array load (kind %u)", (unsigned)kind);

  // Every register this runs on was chosen by getByIndexImpl(); see
  // GetByIndexRegs. Nothing here allocates or frees.
  const a64::GpX &xSource = regs.source;
  // Holds the object, then the buffer, then the element base address.
  const a64::GpX &xLoc = regs.loc;
  const a64::GpX &xTemp1 = regs.temp1;
  // Holds the loaded element, widened to the double it must become.
  const a64::VecD &dValTmp = regs.valTmp;
  const a64::GpX &xRes = regs.res;

  // The element width of this kind, in bytes -- what K has to be scaled by
  // to become the constant displacement below. It comes from the same
  // helper the shared tail cross-checks the address with, so the two cannot
  // disagree.
  const uint32_t width = 1u << typedArrayLogWidth(kind);

  // The object and exact-kind checks come FIRST, for the same reason they
  // do in emitGetByValTypedArrayTier(): at a duo site a kind miss must
  // reach the JSArray tier, and a non-object cannot be read by either tier,
  // so only the kind check chains.
  emit_sh_ljs_is_object(a, xTemp1, xSource);
  a.b_ne(helperLab);
  emit_sh_ljs_get_pointer(a, xLoc, xSource);
  emit_gccell_get_kind(a, xTemp1, xLoc);
  a.cmp(xTemp1.w(), (uint32_t)kind);
  a.b_ne(kindMissLab);

  // No object-flags check, and no key conversion: the read path checks
  // neither fastIndexProperties nor frozen for a typed array
  // (tryFastGetComputedNoAlloc(), JSObject-inline.h), and K is an emit-time
  // constant in [0, 255], so there is nothing to convert and no parity exit.

  // From here on the answer is this tier's, whatever it is: an
  // out-of-bounds index and a detached buffer both read as `undefined`, so
  // they branch to a local `undefined` rather than to the helper. Nothing
  // is recorded on that path, and nothing should be: the source's shape
  // there IS the specialized kind.
  asmjit::Label undefLab = a.newLabel();
  asmjit::Label doneLab = a.newLabel();

  // Bounds: K < length_, one unsigned 32-bit compare. With K an immediate
  // the compare is against the loaded length DIRECTLY, which reverses both
  // the operand order and the condition of emitGetByValTypedArrayTier()'s
  // `cmp idx, length_` / `b.hs`: `b.ls` is taken exactly when
  // `length_ <= K`, i.e. exactly when K is not a valid index. Acceptance
  // stays STRICT -- K == length_ is out of bounds, and takes the branch.
  // K is a uint8, so it is always a valid `cmp` immediate. This tree has no
  // resizable ArrayBuffers, so length_ is fixed for the object's lifetime.
  static_assert(
      RuntimeOffsets::jsTypedArrayBaseLength % 4 == 0 &&
          RuntimeOffsets::jsTypedArrayBaseLength < maxNaturalBaseOffset(4),
      "the typed array length must be reachable by an unsigned-offset LDR");
  assert(key <= UINT8_MAX && "the GetByIndex key must be a uint8");
  a.ldr(xTemp1.w(), a64::Mem(xLoc, RuntimeOffsets::jsTypedArrayBaseLength));
  a.cmp(xTemp1.w(), key);
  a.b_ls(undefLab);

  // The element base address: the buffer's data_, plus the view's byte
  // offset_ into it. A detached buffer has a null data_, so the attached
  // check is free with a load the read needs anyway. Verbatim from
  // emitGetByValTypedArrayTier(), including its use of a real temp rather
  // than xScratch to carry the offset -- on arm64 xScratch is what mask and
  // immediate materializations use, so nothing may live there across other
  // emitters. The 32-bit load zeroes the upper half of xTemp1, which is what
  // makes the 64-bit add below the zero-extended one.
  static_assert(
      RuntimeOffsets::jsTypedArrayBaseOffset % 4 == 0 &&
          RuntimeOffsets::jsTypedArrayBaseOffset < maxNaturalBaseOffset(4),
      "the typed array offset must be reachable by an unsigned-offset LDR");
  static_assert(
      RuntimeOffsets::jsTypedArrayBaseBuffer % sizeof(CompressedPointer) == 0 &&
          RuntimeOffsets::jsTypedArrayBaseBuffer <
              maxNaturalBaseOffset(sizeof(CompressedPointer)),
      "the typed array buffer must be reachable by an unsigned-offset LDR");
  static_assert(
      RuntimeOffsets::jsArrayBufferData % 8 == 0 &&
          RuntimeOffsets::jsArrayBufferData < maxNaturalBaseOffset(8),
      "the buffer data pointer must be reachable by an unsigned-offset LDR");
  a.ldr(xTemp1.w(), a64::Mem(xLoc, RuntimeOffsets::jsTypedArrayBaseOffset));
  emit_load_cp(a, xLoc, a64::Mem(xLoc, RuntimeOffsets::jsTypedArrayBaseBuffer));
  emit_sh_cp_decode_non_null(a, xLoc);
  a.ldr(xLoc, a64::Mem(xLoc, RuntimeOffsets::jsArrayBufferData));
  a.cbz(xLoc, undefLab);
  a.add(xLoc, xLoc, xTemp1);

  // The element access itself, through the tail shared with the ByVal tier.
  // Unlike that tier's base+index operand this one has NO index register at
  // all: the element index is a constant, so `K * width` becomes the
  // IMMEDIATE displacement here and the tail has nothing left to scale.
  // K <= 255 and width <= 8, so the product is at most 2040, a multiple of
  // the width and inside every width's scaled 12-bit range.
  emitTypedArrayElementLoad(
      kind,
      a64::Mem(xLoc, (int32_t)(key * width)),
      xRes,
      xTemp1,
      dValTmp,
      doneLab);

  a.bind(undefLab);
  loadBits64InGp(xRes, (uint64_t)_sh_ljs_undefined().raw, "undefined");
  a.bind(doneLab);
}

void Emitter::getByIndexImpl(FR frRes, FR frSource, uint32_t key) {
  // The GetByIndex instruction's own bytecode offset, unique within this
  // function and stable across recompiles: identifies this site's entry in
  // versionData_'s consumer records (see JitFunctionData.h). GetByIndex is
  // a distinct opcode from GetByVal, so sharing byValSites with
  // the ByVal sites can never collide.
  uint32_t siteId =
      (uint32_t)((const char *)emittingIP - (const char *)codeBlock_->begin());

  comment("// getByIdx r%u, r%u, %u", frRes.index(), frSource.index(), key);

  syncAllFRTempExcept(frRes != frSource ? frRes : FR());
  syncToFrame(frSource);

  // Monotone tier selection, exactly getByValImpl()'s -- see its comment
  // for why the JSArray tier is a static prior rather than a decision, and
  // why selecting the typed-array tier from the observed field alone is
  // already monotone. The predicate is the LOAD one, which admits
  // Uint8Clamped where the store predicate does not; the recording helper
  // for this site validated the kind against the same predicate before
  // storing it.
  JitByValSiteRecord &site = byValSiteRecord(siteId);
  const bool emitTA = site.taKind != JitByValSiteRecord::kTAKindNone &&
      isJitSupportedTypedArrayLoadKind((CellKind)site.taKind);
  site.specializedTAKind =
      emitTA ? site.taKind : JitByValSiteRecord::kTAKindNone;

  asmjit::Label helperLab = a.newLabel();
  asmjit::Label contLab = a.newLabel();

  // The register assignment for the whole tier chain, made once (see
  // GetByIndexRegs) -- the only register-allocator bookkeeping in the
  // sequence, exactly as getByValImpl() does it. The allocs and frees below
  // emit no code at all: they only decide which registers the tiers may
  // use. There is no key register: K is an emit-time constant.
  freeAllFRTempExcept(frSource);
  // The source is needed as a raw HermesValue in a GP register.
  HWReg hwSource = getOrAllocFRInGpX(frSource, true);
  HWReg hwLoc = allocTempGpX();
  HWReg hwIdx = allocTempGpX();
  HWReg hwTemp1 = allocTempGpX();
  HWReg hwValTmp = emitTA ? allocTempVecD() : HWReg{};
  freeReg(hwLoc);
  freeReg(hwIdx);
  freeReg(hwTemp1);
  if (emitTA)
    freeReg(hwValTmp);
  freeFRTemp(frSource);
  // The result register comes last, so it may overlap the temps just freed
  // -- the same ordering getByValImpl() uses for hwRes. The gpX(0) hint is
  // what lets the slow path (whose helper call returns in x0) reconcile with
  // this register at no cost in the common case. `false`: this is a fresh
  // output, no current value to load.
  HWReg hwRes = getOrAllocFRInGpX(frRes, false, HWReg::gpX(0));

  GetByIndexRegs regs;
  regs.source = hwSource.a64GpX();
  regs.loc = hwLoc.a64GpX();
  regs.idx = hwIdx.a64GpX();
  regs.temp1 = hwTemp1.a64GpX();
  if (emitTA)
    regs.valTmp = hwValTmp.a64VecD();
  regs.res = hwRes.a64GpX();
  // res may share a register with loc or idx -- both are dead by the time a
  // tier writes it -- but NOT with temp1, which the fast array tier still
  // needs after loading the element into res (see its hole check). The
  // allocation above guarantees it: res is either a global register, which
  // is never a temp, or x0, which is free here and therefore was handed out
  // as the FIRST temp, loc.
  assert(regs.temp1 != regs.res && "the result must not alias temp1");

  if (emitTA) {
    // The recorded kind is compared first: it is what the site was observed
    // declining on. Its kind miss chains into the JSArray tier below, never
    // into the helper.
    asmjit::Label kindMissLab = a.newLabel();
    emitGetByIndexTypedArrayTier(
        regs, key, (CellKind)site.taKind, kindMissLab, helperLab);
    // Falling out of the inline tier means the load is done.
    a.b(contLab);
    a.bind(kindMissLab);
  }
  emitGetByIndexFastArrayTier(
      regs, key, helperLab, /* sourceKnownObject */ emitTA);
  // Falling out of the inline tier means the load is done.
  a.b(contLab);
  a.bind(helperLab);

  // No freeAllFRTempExcept({}) here: the prologue above already left every
  // temp free and no FR but frRes registered in one, and frRes's register
  // must survive into the call below so the slow-path result lands in the
  // same register the tier used -- the same structure getByValImpl() uses
  // between its fast path and its shared call.
  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frSource);
  // The key goes into w2 BY VALUE, as an immediate: _jit_get_by_index takes
  // the uint32 itself, matching the plain helper it forwards to, not a
  // pointer to a frame slot the way _jit_get_by_val's key does.
  a.mov(a64::w2, key);
  loadBits64InGp(a64::x3, (uint64_t)versionData_, "JitVersionData");
  a.mov(a64::w4, siteId);

  // site.helper is per-site mutable state (spec: "Slow-path demotion: the
  // pointer flip"): it starts out null on a fresh record and is carried
  // forward as-is across recompiles, so a demoted slot (flipped by the
  // runtime to the plain _sh_ljs_get_by_index_rjs) stays demoted. Only a
  // still-null slot -- never yet flipped -- is initialized here. An
  // unconditional store would silently restart recording at every
  // recompile.
  if (!site.helper)
    site.helper = (void *)_jit_get_by_index;
  // Type-check the recording callee against the signature the emitted call
  // sequence assumes, the same way EMIT_RUNTIME_CALL does for a direct call
  // -- even though it is not called directly here, since the actual callee
  // is loaded from site.helper at runtime. The demoted callee takes three
  // arguments rather than five; under AAPCS64 the extra argument registers
  // are simply ignored, exactly as for the ByVal sites'.
  using _FnT = SHLegacyValue (*)(
      SHRuntime *, SHLegacyValue *, uint32_t, SHJitVersionData *, uint32_t);
  _FnT _fn = _jit_get_by_index;
  (void)_fn;
  // WithSavedIP, not the plain indirect call: the fallback resolves the
  // property, which may run a getter and may throw.
  callRuntimeWithSavedIPIndirect(
      (uint64_t)&site.helper, "_jit_get_by_index [indirect]");

  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);

  a.bind(contLab);
}

void Emitter::getByIndex(FR frRes, FR frSource, uint32_t key) {
  getByIndexImpl(frRes, frSource, key);
}

#if HERMES_JIT_INLINE_SAFE_STORE
void Emitter::emitPutByIdInlineTier(
    FR frTarget,
    FR frValue,
    uint16_t clazzID,
    SlotIndex slot,
    const asmjit::Label &helperLab) {
  comment("// Put to object specialization");

  // Both operands are already synced to the frame by the caller, so any temp
  // holding another FR is dead weight here.
  freeAllFRTempExcept(frTarget);

  // We need the target and the value in GP registers.
  HWReg hwTarget = getOrAllocFRInGpX(frTarget, true);
  HWReg hwValue = getOrAllocFRInGpX(frValue, true);

  // As in GetByIdImpl::emitFastPath(), the allocs and frees below emit no
  // code at all: they only decide which registers this sequence may use,
  // based on what we know about the live ranges. Once every register is
  // recorded, all of them are marked free, which is what leaves the helper
  // path below with no temp registered as an FR location.
  HWReg hwLoc = allocTempGpX();
  HWReg hwTemp1 = allocTempGpX();
  HWReg hwTemp2 = allocTempGpX();
  const a64::GpX xTarget = hwTarget.a64GpX();
  const a64::GpX xValue = hwValue.a64GpX();
  const a64::GpX xLoc = hwLoc.a64GpX();
  const a64::GpX xTemp1 = hwTemp1.a64GpX();
  const a64::GpX xTemp2 = hwTemp2.a64GpX();
  freeReg(hwLoc);
  freeReg(hwTemp1);
  freeReg(hwTemp2);
  freeFRTemp(frTarget);
  freeFRTemp(frValue);

  // Code generation starts here.

  // The value has to be encoded into the SmallHermesValue a property slot
  // actually holds, and one value cannot be: a double that would need a
  // heap-allocated BoxedDouble is declined here. Doing that FIRST, before any
  // guard, is what makes the decline cheap -- it skips the whole chain below
  // rather than running it and then giving up. xTemp2 holds the result from
  // here to the store; no guard touches it. In the default heap-value mode
  // this emits nothing at all -- not even the comment, which the encoder
  // itself writes -- and `xShv` is `xValue`.
  const a64::GpX &xShv =
      emit_shv_encode_for_slot_or_slow(a, xTemp2, xValue, xTemp1, helperLab);

  // Is the target an object?
  emit_sh_ljs_is_object(a, xTemp1, xTarget);
  a.b_ne(helperLab);
  // xLoc is the pointer to the object.
  emit_sh_ljs_get_pointer(a, xLoc, xTarget);

  // xTemp1 is the hidden class.
  emit_load_cp(a, xTemp1, a64::Mem(xLoc, offsetof(SHJSObject, clazz)));
  emit_sh_cp_decode_non_null(a, xTemp1);
  // xTemp1 = hc->lazyJITId, a zero-extending 16-bit load.
  a.ldrh(xTemp1.w(), a64::Mem(xTemp1, RuntimeOffsets::hiddenClassLazyJITId));
  // xScratch, not xTemp2: xTemp2 is carrying the encoded value to the store.
  emit_cmp_imm32(a, xTemp1.w(), clazzID, xScratch.w());
  a.b_ne(helperLab);

  // The class matches, so the object has the cached slot as a plain own data
  // property -- the same conclusion _jit_put_by_id draws from the same
  // comparison. Turn xLoc into the address of that slot; the arithmetic is
  // GetByIdImpl::emitLoadFromSlot()'s, one step short of the load.
  size_t ofs;
  if (slot < HERMESVM_DIRECT_PROPERTY_SLOTS) {
    ofs = offsetof(SHJSObjectAndDirectProps, directProps) +
        (size_t)slot * sizeof(SHGCSmallHermesValue);
  } else {
    emit_load_cp(a, xLoc, a64::Mem(xLoc, offsetof(SHJSObject, propStorage)));
    emit_sh_cp_decode_non_null(a, xLoc);
    ofs = offsetof(SHArrayStorageSmall, storage) +
        (size_t)(slot - HERMESVM_DIRECT_PROPERTY_SLOTS) *
            sizeof(SHGCSmallHermesValue);
  }
  // WritePropertyCacheEntry::kMaxSlot bounds this at a couple of kilobytes,
  // so the 24-bit form is more than enough and no scratch register is needed.
  assert(ofs <= 0xFFFFFF && "slot offset must fit emit_add_imm_u24()");
  emit_add_imm_u24(a, xLoc, (uint32_t)ofs);

  // Store, if the write barrier for it is a no-op or a card-dirty.
  emitSafeStoreOrSlow(xLoc, xShv, xValue, xTemp1, xTemp2, helperLab);
}
#endif // HERMES_JIT_INLINE_SAFE_STORE

void Emitter::putByIdImpl(
    FR frTarget,
    SHSymbolID symID,
    FR frValue,
    uint8_t cacheIdx,
    bool strictMode,
    bool tryProp) {
  comment(
      "// %sPutById%s r%u, r%u, cache %u, symID %u",
      tryProp ? "Try" : "",
      strictMode ? "Strict" : "Loose",
      frTarget.index(),
      frValue.index(),
      cacheIdx,
      symID);

  // New non-dictionary objects must not have their propStorage_ capacity
  // stored out of line, because we want to be able to quickly get property
  // storage capacity for JIT's cached add property fast path.
  // Ensure this by making sure that objects will be turned into dictionary
  // mode before leaving the single young gen segment.
  // TODO: Actually implement the fast path. This assert just keeps it possible.
  static_assert(
      HiddenClass::kDictionaryThreshold <
          JSObject::maxYoungGenAllocationPropCount(),
      "dictionary objects must be allocated in a single segment in young gen");

#if HERMES_JIT_INLINE_SAFE_STORE
  // Decide, at compile time, whether an inline tier goes ahead of the helper
  // call. It does when the site's write cache already recorded a hidden
  // class, i.e. when this site has run interpreted and hit -- which is what
  // makes the guard below likely to succeed. A cold cache (every site under
  // -Xjit=force) emits the helper call alone, exactly as before.
  //
  // The cached class is the class of the object for a write to an EXISTING
  // property, so class equality alone proves the slot, and that is precisely
  // the conclusion _jit_put_by_id's own fast path draws. strictMode and
  // tryProp do not enter into it: they only matter when the property is
  // missing or unwritable, which the class match rules out. They are still
  // passed to the helper, which handles every other case.
  uint16_t clazzID = 0;
  SlotIndex slot = 0;
  if (cacheIdx != hbc::PROPERTY_CACHING_DISABLED) {
    WritePropertyCacheEntry *cacheEntry =
        codeBlock_->getWriteCacheEntry(cacheIdx);
    slot = cacheEntry->getSlot();
    HiddenClass *cachedClazz =
        cacheEntry->clazz.get(runtime_, runtime_.getHeap());
    // A valid cache index with no cached class is a site a recompile can
    // upgrade once the cache warms. Cold means the cache names no class
    // yet -- do NOT use clazzID for this: initHCLazyIDMayAlloc() also
    // returns 0 for a warm class when the lazy-ID space is exhausted, and
    // such a site can never be specialized by a recompile; advertising it
    // as a warming opportunity burns the recompile budget on identical
    // bodies.
    if (!cachedClazz)
      coldWriteCacheIdxs_.push_back(cacheIdx);
    // NOTE: initHCLazyIDMayAlloc() is a GC safepoint -- it may create or grow
    // the usedHCs ArrayStorage -- so the class pointer it is handed must not
    // be used afterwards. Only the returned id is, and a non-zero id means
    // the class is pinned in usedHCs and will outlive this compiled function.
    clazzID = initHCLazyIDMayAlloc(cachedClazz);
  }
  asmjit::Label helperLab;
  asmjit::Label contLab;
#endif

  syncAllFRTempExcept({});
  syncToFrame(frTarget);
  syncToFrame(frValue);

#if HERMES_JIT_INLINE_SAFE_STORE
  if (clazzID) {
    helperLab = a.newLabel();
    contLab = a.newLabel();
    emitPutByIdInlineTier(frTarget, frValue, clazzID, slot, helperLab);
    // Falling out of the inline tier means the store is done.
    a.b(contLab);
    a.bind(helperLab);
  }
#endif

  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadBits64InGp(a64::x1, (uint64_t)versionData_, "JitVersionData");
  loadFrameAddr(a64::x2, frTarget);
  loadFrameAddr(a64::x3, frValue);
  a.mov(a64::w4, cacheIdx);
  a.mov(a64::w5, symID);
  a.mov(a64::w6, strictMode);
  a.mov(a64::w7, tryProp);
  EMIT_RUNTIME_CALL(
      *this,
      void (*)(
          SHRuntime *shr,
          SHJitVersionData *versionData,
          SHLegacyValue *base,
          SHLegacyValue *value,
          uint8_t cacheIdx,
          SHSymbolID symID,
          bool strictMode,
          bool tryProp),
      _jit_put_by_id);

#if HERMES_JIT_INLINE_SAFE_STORE
  if (contLab.isValid())
    a.bind(contLab);
#endif
}

void Emitter::defineOwnById(
    FR frTarget,
    SHSymbolID symID,
    FR frValue,
    uint8_t cacheIdx) {
  comment(
      "// defineOwnById r%u, r%u, cache %u, symID %u",
      frTarget.index(),
      frValue.index(),
      cacheIdx,
      symID);

  syncAllFRTempExcept({});
  syncToFrame(frTarget);
  syncToFrame(frValue);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frTarget);
  a.mov(a64::w2, symID);
  loadFrameAddr(a64::x3, frValue);
  if (cacheIdx == hbc::PROPERTY_CACHING_DISABLED) {
    a.mov(a64::x4, 0);
  } else {
    a.ldr(a64::x4, a64::Mem(roDataLabel_, roOfsWritePropertyCachePtr_));
    if (cacheIdx != 0)
      a.add(a64::x4, a64::x4, sizeof(SHWritePropertyCacheEntry) * cacheIdx);
  }
  EMIT_RUNTIME_CALL(
      *this,
      void (*)(
          SHRuntime *shr,
          SHLegacyValue *target,
          SHSymbolID key,
          SHLegacyValue *value,
          SHWritePropertyCacheEntry *cacheEntrySHRuntime),
      _sh_ljs_define_own_by_id);
}

void Emitter::defineOwnInDenseArray(FR frArray, FR frProp, uint32_t idx) {
  comment(
      "// DefineOwnInDenseArray r%u, r%u, %u",
      frArray.index(),
      frProp.index(),
      idx);

  syncAllFRTempExcept({});
  syncToFrame(frArray);
  syncToFrame(frProp);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frArray);
  loadFrameAddr(a64::x2, frProp);
  a.mov(a64::w3, idx);
  EMIT_RUNTIME_CALL(
      *this,
      void (*)(SHRuntime *, SHLegacyValue *, SHLegacyValue *, uint32_t),
      _sh_ljs_define_own_in_dense_array);
}

void Emitter::defineOwnByIndex(FR frTarget, FR frValue, uint32_t key) {
  comment(
      "// putOwnByIdx r%u, r%u, %u", frTarget.index(), frValue.index(), key);

  syncAllFRTempExcept({});
  syncToFrame(frTarget);
  syncToFrame(frValue);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frTarget);
  a.mov(a64::w2, key);
  loadFrameAddr(a64::x3, frValue);
  EMIT_RUNTIME_CALL(
      *this,
      void (*)(SHRuntime *, SHLegacyValue *, uint32_t, SHLegacyValue *),
      _sh_ljs_define_own_by_index);
}

void Emitter::defineOwnByVal(
    FR frTarget,
    FR frValue,
    FR frKey,
    bool enumerable) {
  comment(
      "// DefineOwnByVal r%u, r%u, r%u",
      frTarget.index(),
      frValue.index(),
      frKey.index());

  syncAllFRTempExcept({});
  syncToFrame(frTarget);
  syncToFrame(frValue);
  syncToFrame(frKey);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frTarget);
  loadFrameAddr(a64::x2, frKey);
  loadFrameAddr(a64::x3, frValue);
  if (enumerable) {
    EMIT_RUNTIME_CALL(
        *this,
        void (*)(
            SHRuntime *, SHLegacyValue *, SHLegacyValue *, SHLegacyValue *),
        _sh_ljs_define_own_by_val);
  } else {
    EMIT_RUNTIME_CALL(
        *this,
        void (*)(
            SHRuntime *, SHLegacyValue *, SHLegacyValue *, SHLegacyValue *),
        _sh_ljs_define_own_ne_by_val);
  }
}

void Emitter::defineOwnGetterSetterByVal(
    FR frTarget,
    FR frKey,
    FR frGetter,
    FR frSetter,
    bool enumerable) {
  comment(
      "// DefineOwnGetterSetterByVal r%u, r%u, r%u, r%u, %d",
      frTarget.index(),
      frKey.index(),
      frGetter.index(),
      frSetter.index(),
      enumerable);

  syncAllFRTempExcept({});
  syncToFrame(frTarget);
  syncToFrame(frKey);
  syncToFrame(frGetter);
  syncToFrame(frSetter);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frTarget);
  loadFrameAddr(a64::x2, frKey);
  loadFrameAddr(a64::x3, frGetter);
  loadFrameAddr(a64::x4, frSetter);
  a.mov(a64::w5, enumerable);

  EMIT_RUNTIME_CALL(
      *this,
      void (*)(
          SHRuntime *shr,
          SHLegacyValue *target,
          SHLegacyValue *key,
          SHLegacyValue *getter,
          SHLegacyValue *setter,
          bool enumerable),
      _sh_ljs_define_own_getter_setter_by_val);
}

void Emitter::getOwnBySlotIdx(FR frRes, FR frTarget, uint32_t slotIdx) {
  comment(
      "// GetOwnBySlotIdx r%u, r%u, %u",
      frRes.index(),
      frTarget.index(),
      slotIdx);

  HWReg hwTarget = getOrAllocFRInGpX(frTarget, true);
  HWReg hwRes = getOrAllocFRInGpX(frRes, false);
  frUpdatedWithHW(frRes, hwRes);
  a64::GpX xRes = hwRes.a64GpX();

  size_t ofs;
  emit_sh_ljs_get_pointer(a, xRes, hwTarget.a64GpX());
  if (slotIdx < JSObject::DIRECT_PROPERTY_SLOTS) {
    // If the slot is in the direct property slots, load it directly.
    ofs = offsetof(SHJSObjectAndDirectProps, directProps) +
        slotIdx * sizeof(SHGCSmallHermesValue);
  } else {
    // If the slot is in indirect storage, retrieve the pointer to that storage.
    emit_load_cp(a, xRes, a64::Mem(xRes, offsetof(SHJSObject, propStorage)));
    emit_sh_cp_decode_non_null(a, xRes);
    auto storageSlot = slotIdx - JSObject::DIRECT_PROPERTY_SLOTS;
    ofs = offsetof(SHArrayStorageSmall, storage) +
        storageSlot * sizeof(SHGCSmallHermesValue);
  }
  emit_load_shv(a, xRes, a64::Mem(xRes, ofs));
  auto doneLab = a.newLabel();
  emit_sh_shv_decode(a, xRes, doneLab);
  a.bind(doneLab);
}

void Emitter::putOwnBySlotIdx(FR frTarget, FR frValue, uint32_t slotIdx) {
  comment(
      "// PutOwnBySlotIdx r%u, r%u, %u",
      frTarget.index(),
      frValue.index(),
      slotIdx);

  syncAllFRTempExcept({});
  syncToFrame(frTarget);
  syncToFrame(frValue);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frTarget);
  // For indirect stores, 0 is the first indirect index.
  a.mov(
      a64::w2,
      slotIdx < JSObject::DIRECT_PROPERTY_SLOTS
          ? slotIdx
          : slotIdx - JSObject::DIRECT_PROPERTY_SLOTS);
  loadFrameAddr(a64::x3, frValue);

  if (slotIdx < JSObject::DIRECT_PROPERTY_SLOTS) {
    EMIT_RUNTIME_CALL(
        *this,
        void (*)(SHRuntime *, SHLegacyValue *, uint32_t, SHLegacyValue *),
        _sh_prstore_direct);
  } else {
    EMIT_RUNTIME_CALL(
        *this,
        void (*)(SHRuntime *, SHLegacyValue *, uint32_t, SHLegacyValue *),
        _sh_prstore_indirect);
  }
}

void Emitter::isIn(FR frRes, FR frLeft, FR frRight) {
  comment(
      "// isIn r%u, r%u, r%u", frRes.index(), frLeft.index(), frRight.index());

  syncAllFRTempExcept(frRes != frLeft && frRes != frRight ? frRes : FR());
  syncToFrame(frLeft);
  syncToFrame(frRight);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frLeft);
  loadFrameAddr(a64::x2, frRight);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(SHRuntime *, SHLegacyValue *, SHLegacyValue *),
      _sh_ljs_is_in_rjs);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::privateIsIn(
    FR frRes,
    FR frPrivateName,
    FR frTarget,
    uint8_t cacheIdx) {
  comment(
      "// PrivateIsIn r%u, r%u, r%u, cache %u",
      frRes.index(),
      frPrivateName.index(),
      frTarget.index(),
      cacheIdx);

  syncAllFRTempExcept(
      frRes != frPrivateName && frRes != frTarget ? frRes : FR());
  syncToFrame(frPrivateName);
  syncToFrame(frTarget);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frPrivateName);
  loadFrameAddr(a64::x2, frTarget);

  if (cacheIdx == hbc::PROPERTY_CACHING_DISABLED) {
    a.mov(a64::x3, 0);
  } else {
    a.ldr(a64::x3, a64::Mem(roDataLabel_, roOfsPrivateNameCachePtr_));
    if (cacheIdx != 0)
      a.add(a64::x3, a64::x3, sizeof(SHPrivateNameCacheEntry) * cacheIdx);
  }

  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(
          SHRuntime *,
          SHLegacyValue *,
          SHLegacyValue *,
          SHPrivateNameCacheEntry *),
      _sh_ljs_private_is_in_rjs);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::createPrivateName(FR frRes, SHSymbolID symID) {
  comment("// CreatePrivateName r%u, %u", frRes.index(), symID);
  syncAllFRTempExcept(frRes);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  a.mov(a64::w1, symID);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(SHRuntime *, SHSymbolID),
      _sh_ljs_create_private_name);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

} // namespace hermes::vm::arm64

#endif // HERMESVM_JIT_ARM64

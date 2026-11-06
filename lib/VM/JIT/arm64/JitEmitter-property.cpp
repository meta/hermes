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
    const asmjit::Label &helperLab) {
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

  // Unlike x86-64 there is no SmallHermesValue encode ahead of the guards:
  // HERMES_JIT_INLINE_SAFE_STORE excludes compressed pointers and boxed
  // doubles on arm64 until that encoder is ported (spec stage 5c), so a slot
  // holds the HermesValue in xValue verbatim. The static_assert at the hole
  // test below is what fails the build when that gate opens.

  // Code generation starts here.

  // Is the target an object?
  emit_sh_ljs_is_object(a, xTemp1, xTarget);
  a.b_ne(helperLab);
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
  // materialized in a register first. The value it is compared against is a
  // single bit and encodes directly.
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
  a.mov(xTemp2.w(), flagsMask);
  a.and_(xTemp1.w(), xTemp1.w(), xTemp2.w());
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
      a, xTemp1.w(), RuntimeOffsets::kMaxInlineStorage - 1, xTemp2.w());
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
  // In this build state a slot holds the HermesValue's bits unshifted, so
  // the test is HermesValue's own ETag test. The static_assert is what fails
  // the build when the arm64 gate opens to either of the two modes stage 5c
  // adds: under HERMESVM_COMPRESSED_POINTERS a slot is a 4-byte
  // HermesValue32, and under HERMESVM_BOXED_DOUBLES alone it is an 8-byte
  // one -- same width as here, but with the tag in the LOW bits, so a size
  // comparison would not have caught it. Both need the whole encoded empty
  // value compared instead, as emit_shv_load_is_empty() does on x86-64.
  static_assert(
      std::is_same_v<SmallHermesValue, SmallHermesValueAdaptor>,
      "an element slot holds a HermesValue verbatim in this build state");
  a.ldr(xTemp1, a64::Mem(xLoc));
  emit_sh_ljs_is_empty(a, xTemp1, xTemp1);
  a.b_eq(helperLab);

  // Store, if the write barrier for it is a no-op or a card-dirty.
  emitSafeStoreOrSlow(xLoc, xValue, xTemp1, xTemp2, helperLab);
}
#endif // HERMES_JIT_INLINE_SAFE_STORE

void Emitter::putByValImpl(
    FR frTarget,
    FR frKey,
    FR frValue,
    const char *name,
    void (*shImpl)(
        SHRuntime *shr,
        SHLegacyValue *target,
        SHLegacyValue *key,
        SHLegacyValue *value),
    const char *shImplName) {
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

#if HERMES_JIT_INLINE_SAFE_STORE
  // Unlike the PutById tier this needs nothing from a property cache: the
  // guards are all on the values themselves, so the tier goes ahead of the
  // helper call unconditionally. PutByVal passes the target as the receiver,
  // which is the one precondition of the runtime fast path that is satisfied
  // by construction rather than by a guard.
  asmjit::Label helperLab = a.newLabel();
  asmjit::Label contLab = a.newLabel();
  emitPutByValFastArrayTier(frTarget, frKey, frValue, helperLab);
  // Falling out of the inline tier means the store is done.
  a.b(contLab);
  a.bind(helperLab);
#endif

  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frTarget);
  loadFrameAddr(a64::x2, frKey);
  loadFrameAddr(a64::x3, frValue);
  callRuntimeWithSavedIP((void *)shImpl, shImplName);

#if HERMES_JIT_INLINE_SAFE_STORE
  a.bind(contLab);
#endif
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
      const char *shImplName)
      : _(emitter),
        a(emitter.a),
        frRes(frRes),
        symID(symID),
        frSource(frSource),
        cacheIdx(cacheIdx),
        name(name),
        shImpl(shImpl),
        shImplName(shImplName) {}

  void run() {
    _.comment(
        "// %s r%u, r%u, cache %u, symID %u",
        name,
        frRes.index(),
        frSource.index(),
        cacheIdx,
        symID);

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
    _.callRuntimeWithSavedIP((void *)shImpl, shImplName);

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

    // Optionally emit a very fast path specialized for the cache entry, if the
    // entry had exactly one successful match.
    if (ReadPropertyCacheEntry *cacheEntry =
            _.codeBlock_->getReadCacheEntry(cacheIdx);
        cacheEntry->numGoodChanges == 1) {
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
    const char *shImplName) {
  GetByIdImpl(*this, frRes, symID, frSource, cacheIdx, name, shImpl, shImplName)
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

void Emitter::getByVal(FR frRes, FR frSource, FR frKey) {
  comment(
      "// getByVal r%u, r%u, r%u",
      frRes.index(),
      frSource.index(),
      frKey.index());

  syncAllFRTempExcept(frRes != frSource && frRes != frKey ? frRes : FR());
  syncToFrame(frSource);
  syncToFrame(frKey);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frSource);
  loadFrameAddr(a64::x2, frKey);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(SHRuntime *, SHLegacyValue *, SHLegacyValue *),
      _sh_ljs_get_by_val_rjs);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::getByIndex(FR frRes, FR frSource, uint32_t key) {
  comment("// getByIdx r%u, r%u, %u", frRes.index(), frSource.index(), key);

  syncAllFRTempExcept(frRes != frSource ? frRes : FR());
  syncToFrame(frSource);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frSource);
  a.mov(a64::w2, key);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(SHRuntime *, SHLegacyValue *, uint32_t),
      _sh_ljs_get_by_index_rjs);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
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
  emit_cmp_imm32(a, xTemp1.w(), clazzID, xTemp2.w());
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
  emitSafeStoreOrSlow(xLoc, xValue, xTemp1, xTemp2, helperLab);
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
    // NOTE: initHCLazyIDMayAlloc() is a GC safepoint -- it may create or grow
    // the usedHCs ArrayStorage -- so the class pointer it is handed must not
    // be used afterwards. Only the returned id is, and a non-zero id means
    // the class is pinned in usedHCs and will outlive this compiled function.
    clazzID = initHCLazyIDMayAlloc(
        cacheEntry->clazz.get(runtime_, runtime_.getHeap()));
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
  loadBits64InGp(a64::x1, (uint64_t)codeBlock_, "CodeBlock");
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
          SHCodeBlock *codeBlock,
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

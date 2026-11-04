/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JIT/Config.h"
#if HERMESVM_JIT_X86_64
#include "../JitHandlers.h"
#include "JitEmitter-internal.h"
#include "JitEmitter.h"

#include "hermes/VM/JSObject-inline.h"
#include "llvh/ADT/Statistic.h"

#define DEBUG_TYPE "jit"

STATISTIC(
    JITNumGetByIdSpec,
    "JITNumGetByIdSpec: number of GetById specialized fast paths emitted");

namespace hermes::vm::x86_64 {

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
  freeAllFRTempExcept({});

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frTarget);
  loadFrameAddr(x86::rdx, frKey);
  loadFrameAddr(x86::rcx, frValue);
  callRuntimeWithSavedIP((void *)shImpl, shImplName);
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

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frTarget);
  loadFrameAddr(x86::rdx, frKey);
  loadFrameAddr(x86::rcx, frValue);
  loadFrameAddr(x86::r8, frReceiver);
  a.mov(x86::r9d, asmjit::Imm(isStrict));
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

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frTarget);
  loadFrameAddr(x86::rdx, frKey);
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

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frTarget);
  loadFrameAddr(x86::rdx, frKey);
  loadFrameAddr(x86::rcx, frValue);
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

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frTarget);
  loadFrameAddr(x86::rdx, frKey);

  // x86-64: the RO data entry is addressed RIP-relative, so unlike arm64 this
  // is a plain load with no base register to set up, and the cache offset
  // folds into a single add with an imm32. See createThis().
  if (cacheIdx == hbc::PROPERTY_CACHING_DISABLED) {
    a.xor_(x86::ecx, x86::ecx);
  } else {
    a.mov(x86::rcx, x86::qword_ptr(roDataLabel_, roOfsPrivateNameCachePtr_));
    if (cacheIdx != 0)
      a.add(x86::rcx, asmjit::Imm(sizeof(SHPrivateNameCacheEntry) * cacheIdx));
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

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frTarget);
  loadFrameAddr(x86::rdx, frKey);
  loadFrameAddr(x86::rcx, frValue);

  // See getOwnPrivateBySym() for the RIP-relative cache pointer load.
  if (cacheIdx == hbc::PROPERTY_CACHING_DISABLED) {
    a.xor_(x86::r8d, x86::r8d);
  } else {
    a.mov(x86::r8, x86::qword_ptr(roDataLabel_, roOfsPrivateNameCachePtr_));
    if (cacheIdx != 0)
      a.add(x86::r8, asmjit::Imm(sizeof(SHPrivateNameCacheEntry) * cacheIdx));
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
  x86::Assembler &a;

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
  x86::Gp temp1;
  x86::Gp temp2;
  x86::Gp temp3;
  x86::Gp temp4;

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

    a.mov(x86::rdi, xRuntime);
    _.loadFrameAddr(x86::rsi, frSource);
    a.mov(x86::edx, asmjit::Imm(symID));
    // x86-64: the RO data entry is addressed RIP-relative, so unlike arm64
    // this is a plain load with no base register to set up, and the cache
    // offset folds into a single add with an imm32.
    if (cacheIdx == hbc::PROPERTY_CACHING_DISABLED) {
      a.xor_(x86::ecx, x86::ecx);
    } else {
      a.mov(
          x86::rcx,
          x86::qword_ptr(_.roDataLabel_, _.roOfsReadPropertyCachePtr_));
      if (cacheIdx != 0)
        a.add(
            x86::rcx, asmjit::Imm(sizeof(SHReadPropertyCacheEntry) * cacheIdx));
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
  /// \param objReg The register containing the object pointer. It is
  ///   clobbered when the slot is an indirect one.
  /// \param slot The slot index to load from the object.
  ///
  /// x86-64: arm64 takes a temporary as well, because its scaled load
  /// immediate runs out of range for a large enough slot. Every x86
  /// displacement is a signed 32-bit one, so there is no fallback and no
  /// temporary; the assert below records the remaining constraint.
  void emitLoadFromSlot(
      const x86::Gp &resReg,
      const x86::Gp &objReg,
      SlotIndex slot) {
    size_t ofs;
    if (slot < HERMESVM_DIRECT_PROPERTY_SLOTS) {
      ofs = offsetof(SHJSObjectAndDirectProps, directProps) +
          (size_t)slot * sizeof(SHGCSmallHermesValue);
    } else {
      emit_load_cp(
          a, objReg, x86::ptr(objReg, offsetof(SHJSObject, propStorage)));
      emit_sh_cp_decode_non_null(a, objReg);
      ofs = offsetof(SHArrayStorageSmall, storage) +
          (size_t)(slot - HERMESVM_DIRECT_PROPERTY_SLOTS) *
              sizeof(SHGCSmallHermesValue);
    }
    assert(ofs <= (size_t)INT32_MAX && "slot offset must fit a disp32");
    emit_load_shv(a, resReg, x86::ptr(objReg, (int32_t)ofs));
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

    // temp1 will contain the input object.
    HWReg hwTemp1 = _.allocTempGpX();
    temp1 = hwTemp1.gpq();
    // Free frSource before allocating more temporaries, because we won't
    // need it at the same time as them.
    _.freeFRTemp(frSource);

    // Get register assignments for the rest of the temporaries.
    HWReg hwTemp2 = _.allocTempGpX();
    HWReg hwTemp3 = _.allocTempGpX();
    HWReg hwTemp4 = _.allocTempGpX();
    temp2 = hwTemp2.gpq();
    temp3 = hwTemp3.gpq();
    temp4 = hwTemp4.gpq();

    // Now that we have recorded their registers, mark all temp registers as
    // free.
    _.freeReg(hwTemp1);
    _.freeReg(hwTemp2);
    _.freeReg(hwTemp3);
    _.freeReg(hwTemp4);

    // Allocate the result register. Note that it can overlap the temps we
    // just freed.
    hwRes = _.getOrAllocFRInGpX(frRes, false, HWReg::gpX(0));

    // Finally we begin code generation for the fast path.

    // Is the input an object.
    emit_sh_ljs_is_object(a, temp1, hwSourceGpx.gpq());
    a.jne(slowPathLab);
    // temp1 is the pointer to the object.
    emit_sh_ljs_get_pointer(a, temp1, hwSourceGpx.gpq());

    // temp2 is the hidden class.
    emit_load_cp(a, temp2, x86::ptr(temp1, offsetof(SHJSObject, clazz)));

    Emit_sh_shv_decode shvDecode(a, hwRes.gpq(), contLab);

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

    // temp3 points to the start of read property cache.
    a.mov(temp3, x86::qword_ptr(_.roDataLabel_, _.roOfsReadPropertyCachePtr_));
    // temp4 = cacheEntry->clazz.
    emit_load_cp(
        a,
        temp4,
        x86::ptr(
            temp3,
            (int32_t)(sizeof(SHReadPropertyCacheEntry) * cacheIdx +
                      offsetof(SHReadPropertyCacheEntry, clazz))));

    // Compare hidden classes.
    a.cmp(temp2, temp4);
    a.jne(slowPathLab);

    // Hidden class matches. Fetch the slot in temp4
    emit_load_slot16(a, temp4, temp3, cacheIdx);

    // Is it an indirect slot?
    a.cmp(temp4.r32(), asmjit::Imm(HERMESVM_DIRECT_PROPERTY_SLOTS));
    a.jae(indirectLab);

    // Shift by 2 or 3 bits depending on whether properties are 4 or 8
    // bytes.
    constexpr uint32_t kPropShiftAmt =
        sizeof(SHGCSmallHermesValue) == 4 ? 2 : 3;
    // Load from a direct slot.
    // x86-64: the base of the direct property array is a constant
    // displacement, which folds into the scaled memory operand, so arm64's
    // separate `add` of that offset into a temp is not needed. temp4 was
    // zero-extended by emit_load_slot16(), which is what makes it usable as
    // a 64-bit scaled index.
    emit_load_shv(
        a,
        hwRes.gpq(),
        x86::ptr(
            temp1,
            temp4,
            kPropShiftAmt,
            (int32_t)offsetof(SHJSObjectAndDirectProps, directProps)));
    shvDecode.emitFirstCase(a);
    a.jmp(contLab);

    a.bind(indirectLab);
    // Load from an in-direct slot.
    // temp1 is the object
    // temp4 is the slot

    // temp1 = temp1->propStorage
    emit_load_cp(a, temp1, x86::ptr(temp1, offsetof(SHJSObject, propStorage)));
    emit_sh_cp_decode_non_null(a, temp1);
    // x86-64: the bias that turns a slot index into an offset into the
    // indirect storage is a (possibly negative) constant, and a disp32 is
    // signed, so it folds into the memory operand instead of arm64's
    // separate add/sub.
    constexpr ssize_t ofs = offsetof(SHArrayStorageSmall, storage) -
        HERMESVM_DIRECT_PROPERTY_SLOTS * sizeof(SHGCSmallHermesValue);
    static_assert(
        ofs >= INT32_MIN && ofs <= INT32_MAX, "bias must fit a disp32");
    emit_load_shv(
        a, hwRes.gpq(), x86::ptr(temp1, temp4, kPropShiftAmt, (int32_t)ofs));
    shvDecode.emitAll(a);
    a.jmp(contLab);
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
    const auto &reg =
        emit_sh_cp_decode_non_null_preserve_input(a, temp3, temp2);
    // temp3 = hc->lazyJITId
    // x86-64: a zero-extending 16-bit load, i.e. arm64's ldrh.
    a.movzx(
        temp3.r32(), x86::word_ptr(reg, RuntimeOffsets::hiddenClassLazyJITId));

    // x86-64: arm64 needs a helper (and a scratch register) because a wide
    // immediate does not fit its cmp; a cmp against an imm32 always encodes
    // here, so temp4 is left alone.
    a.cmp(temp3.r32(), asmjit::Imm(clazzID));
    a.jne(failSpecLab);
    // A match. Just load the property directly.
    emitLoadFromSlot(hwRes.gpq(), temp1, cacheEntry->getSlot());
    shvDecode.emitFirstCase(a);
    a.jmp(contLab);
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

    emit_sh_cp_decode_non_null(a, temp2);
    // temp2 = hc->lazyJITId
    a.movzx(
        temp2.r32(),
        x86::word_ptr(temp2, RuntimeOffsets::hiddenClassLazyJITId));
    // if object class mismatch, fail.
    a.cmp(temp2.r32(), asmjit::Imm(clazzID));
    a.jne(failSpecLab);

    // Get the parent, which the object's hidden class holds.
    // x86-64: emit_load_cp() is a plain mov, which writes no flags, so the
    // test below reads exactly the value just loaded -- the same ordering
    // arm64's ldr/cbz pair has.
    emit_load_cp(a, temp1, x86::ptr(temp1, offsetof(SHJSObject, clazz)));
    emit_sh_cp_decode_non_null(a, temp1);
    emit_load_cp(
        a, temp1, x86::ptr(temp1, RuntimeOffsets::hiddenClassObjectParent));
    // If no parent, fail.
    a.test(temp1, temp1);
    a.jz(failSpecLab);
    emit_sh_cp_decode_non_null(a, temp1);
    // Get the parent's hidden class.
    emit_load_cp(a, temp2, x86::ptr(temp1, offsetof(SHJSObject, clazz)));
    emit_sh_cp_decode_non_null(a, temp2);
    // temp2 = hc->lazyJITId
    a.movzx(
        temp2.r32(),
        x86::word_ptr(temp2, RuntimeOffsets::hiddenClassLazyJITId));
    // if parent class mismatch, fail.
    a.cmp(temp2.r32(), asmjit::Imm(parentClsID));
    a.jne(failSpecLab);
    // A match. Just load the property directly.
    emitLoadFromSlot(hwRes.gpq(), temp1, cacheEntry->getSlot());
    shvDecode.emitAll(a);
    a.jmp(contLab);
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

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frSource);
  loadFrameAddr(x86::rdx, frReceiver);
  a.mov(x86::ecx, asmjit::Imm(symID));
  // See getOwnPrivateBySym() for the RIP-relative cache pointer load.
  if (cacheIdx == hbc::PROPERTY_CACHING_DISABLED) {
    a.xor_(x86::r8d, x86::r8d);
  } else {
    a.mov(x86::r8, x86::qword_ptr(roDataLabel_, roOfsReadPropertyCachePtr_));
    if (cacheIdx != 0)
      a.add(x86::r8, asmjit::Imm(sizeof(SHReadPropertyCacheEntry) * cacheIdx));
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

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frSource);
  loadFrameAddr(x86::rdx, frKey);
  loadFrameAddr(x86::rcx, frReceiver);

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

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frSource);
  loadFrameAddr(x86::rdx, frKey);
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

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frSource);
  a.mov(x86::edx, asmjit::Imm(key));
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(SHRuntime *, SHLegacyValue *, uint32_t),
      _sh_ljs_get_by_index_rjs);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

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

  syncAllFRTempExcept({});
  syncToFrame(frTarget);
  syncToFrame(frValue);
  freeAllFRTempExcept({});

  a.mov(x86::rdi, xRuntime);
  loadBits64InGp(x86::rsi, (uint64_t)codeBlock_, "CodeBlock");
  loadFrameAddr(x86::rdx, frTarget);
  loadFrameAddr(x86::rcx, frValue);
  a.mov(x86::r8d, asmjit::Imm(cacheIdx));
  a.mov(x86::r9d, asmjit::Imm(symID));
  // x86-64: this is the one runtime call in the backend with more arguments
  // than SysV has argument registers. arm64 passes all eight in w0-w7; here
  // the last two travel on the stack, at [rsp] and [rsp+8] at the moment of
  // the call. Two pushes move rsp by 16, so the SysV requirement that
  // rsp % 16 == 0 at the call survives them, and nothing between here and
  // the call addresses the frame through rsp -- loadFrameAddr() uses xFrame,
  // and callRuntimeWithSavedIP() only touches xScratch and xRuntime.
  a.push(asmjit::Imm(tryProp));
#ifndef NDEBUG
  rspDelta_ += 8;
#endif
  a.push(asmjit::Imm(strictMode));
#ifndef NDEBUG
  rspDelta_ += 8;
#endif
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
  a.add(x86::rsp, asmjit::Imm(16));
#ifndef NDEBUG
  rspDelta_ -= 16;
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

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frTarget);
  a.mov(x86::edx, asmjit::Imm(symID));
  loadFrameAddr(x86::rcx, frValue);
  // See getOwnPrivateBySym() for the RIP-relative cache pointer load.
  if (cacheIdx == hbc::PROPERTY_CACHING_DISABLED) {
    a.xor_(x86::r8d, x86::r8d);
  } else {
    a.mov(x86::r8, x86::qword_ptr(roDataLabel_, roOfsWritePropertyCachePtr_));
    if (cacheIdx != 0)
      a.add(x86::r8, asmjit::Imm(sizeof(SHWritePropertyCacheEntry) * cacheIdx));
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

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frArray);
  loadFrameAddr(x86::rdx, frProp);
  a.mov(x86::ecx, asmjit::Imm(idx));
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

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frTarget);
  a.mov(x86::edx, asmjit::Imm(key));
  loadFrameAddr(x86::rcx, frValue);
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

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frTarget);
  loadFrameAddr(x86::rdx, frKey);
  loadFrameAddr(x86::rcx, frValue);
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

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frTarget);
  loadFrameAddr(x86::rdx, frKey);
  loadFrameAddr(x86::rcx, frGetter);
  loadFrameAddr(x86::r8, frSetter);
  a.mov(x86::r9d, asmjit::Imm(enumerable));

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
  x86::Gp res = hwRes.gpq();

  size_t ofs;
  emit_sh_ljs_get_pointer(a, res, hwTarget.gpq());
  if (slotIdx < JSObject::DIRECT_PROPERTY_SLOTS) {
    // If the slot is in the direct property slots, load it directly.
    ofs = offsetof(SHJSObjectAndDirectProps, directProps) +
        (size_t)slotIdx * sizeof(SHGCSmallHermesValue);
  } else {
    // If the slot is in indirect storage, retrieve the pointer to that storage.
    emit_load_cp(a, res, x86::ptr(res, offsetof(SHJSObject, propStorage)));
    emit_sh_cp_decode_non_null(a, res);
    auto storageSlot = slotIdx - JSObject::DIRECT_PROPERTY_SLOTS;
    ofs = offsetof(SHArrayStorageSmall, storage) +
        (size_t)storageSlot * sizeof(SHGCSmallHermesValue);
  }
  assert(ofs <= (size_t)INT32_MAX && "slot offset must fit a disp32");
  emit_load_shv(a, res, x86::ptr(res, (int32_t)ofs));
  auto doneLab = a.newLabel();
  emit_sh_shv_decode(a, res, doneLab);
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

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frTarget);
  // For indirect stores, 0 is the first indirect index.
  a.mov(
      x86::edx,
      asmjit::Imm(
          slotIdx < JSObject::DIRECT_PROPERTY_SLOTS
              ? slotIdx
              : slotIdx - JSObject::DIRECT_PROPERTY_SLOTS));
  loadFrameAddr(x86::rcx, frValue);

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

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frLeft);
  loadFrameAddr(x86::rdx, frRight);
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

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frPrivateName);
  loadFrameAddr(x86::rdx, frTarget);

  // See getOwnPrivateBySym() for the RIP-relative cache pointer load.
  if (cacheIdx == hbc::PROPERTY_CACHING_DISABLED) {
    a.xor_(x86::ecx, x86::ecx);
  } else {
    a.mov(x86::rcx, x86::qword_ptr(roDataLabel_, roOfsPrivateNameCachePtr_));
    if (cacheIdx != 0)
      a.add(x86::rcx, asmjit::Imm(sizeof(SHPrivateNameCacheEntry) * cacheIdx));
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

  a.mov(x86::rdi, xRuntime);
  a.mov(x86::esi, asmjit::Imm(symID));
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(SHRuntime *, SHSymbolID),
      _sh_ljs_create_private_name);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

} // namespace hermes::vm::x86_64

#endif // HERMESVM_JIT_X86_64

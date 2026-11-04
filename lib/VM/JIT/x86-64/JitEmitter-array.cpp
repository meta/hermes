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

namespace hermes::vm::x86_64 {

void Emitter::newArray(FR frRes, uint32_t size) {
  comment("// NewArray r%u, %u", frRes.index(), size);
  syncAllFRTempExcept(frRes);
  freeAllFRTempExcept({});
  a.mov(x86::rdi, xRuntime);
  a.mov(x86::esi, asmjit::Imm(size));
  EMIT_RUNTIME_CALL(
      *this, SHLegacyValue (*)(SHRuntime *, uint32_t), _sh_ljs_new_array);
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::newArrayWithBuffer(
    FR frRes,
    uint32_t numElements,
    uint32_t numLiterals,
    uint32_t bufferIndex) {
  comment(
      "// NewArrayWithBuffer r%u, %u, %u, %u",
      frRes.index(),
      numElements,
      numLiterals,
      bufferIndex);

  // x86-64: unlike NewObjectWithBuffer, which walks the literal buffer in
  // emitted code, the array literal buffer is entirely the runtime's business
  // -- this is a plain call, exactly as on arm64.
  syncAllFRTempExcept(frRes);
  freeAllFRTempExcept({});
  a.mov(x86::rdi, xRuntime);
  loadBits64InGp(x86::rsi, (uint64_t)codeBlock_, "CodeBlock");
  a.mov(x86::edx, asmjit::Imm(numElements));
  a.mov(x86::ecx, asmjit::Imm(numLiterals));
  a.mov(x86::r8d, asmjit::Imm(bufferIndex));
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(
          SHRuntime *, SHCodeBlock *, uint32_t, uint32_t, uint32_t),
      _interpreter_create_array_from_buffer);
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::newFastArray(FR frRes, FR frProto, uint32_t size) {
  comment("// NewFastArray r%u, r%u, %u", frRes.index(), frProto.index(), size);
  syncAllFRTempExcept(frRes);
  syncToFrame(frProto);
  freeAllFRTempExcept({});
  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frProto);
  a.mov(x86::edx, asmjit::Imm(size));
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(SHRuntime *, SHLegacyValue *, uint32_t),
      _sh_new_fastarray_with_proto);
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::fastArrayLength(FR frRes, FR frArr) {
  comment("// FastArrayLength r%u, r%u", frRes.index(), frArr.index());
  // We allocate a temporary register to compute the address instead of using
  // the result register in case the result has a VecD allocated for it.
  HWReg temp = allocTempGpX();
  HWReg hwArr = getOrAllocFRInGpX(frArr, true);
  // Done allocating, free the temp so it can be reused for the result.
  freeReg(temp);

  // x86-64: the fast path reinterprets frArr's bits as a pointer, which the
  // FastArrayLength operand's type guarantees. arm64 asserts nothing here;
  // this backend checks the fact the emitted code relies on, per
  // emitTypeAssert's contract. Flags are dead at this point, which that
  // function requires.
  emitTypeAssert(frArr, hwArr, TypePred::IsObject);

  emit_sh_ljs_get_pointer(a, temp.gpq(), hwArr.gpq());

#ifdef HERMESVM_BOXED_DOUBLES
  // If boxed doubles are enabled, load the size from the ArrayStorage, where it
  // is stored as an integer.
  emit_load_cp(
      a,
      temp.gpq(),
      x86::ptr(temp.gpq(), offsetof(SHFastArray, indexedStorage)));
  emit_sh_cp_decode_non_null(a, temp.gpq());
  a.mov(
      temp.gpq().r32(),
      x86::dword_ptr(temp.gpq(), offsetof(SHArrayStorage, size)));
  HWReg hwRes = getOrAllocFRInVecD(frRes, false);
  // x86-64: arm64's ucvtf of a W register; x86 has no unsigned conversion, so
  // this goes through the zero-extended 64-bit value. It clobbers temp, which
  // is dead here.
  emit_int32_to_double(a, hwRes.xmm(), temp.gpq(), /* isUnsigned */ true);
#else
  // If boxed doubles are disabled, we can just load the size from the length
  // property of the FastArray.
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false);
  movHWFromMem(
      hwRes, x86::qword_ptr(temp.gpq(), offsetof(SHFastArray, length)));
#endif

  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::fastArrayLoad(FR frRes, FR frArr, FR frIdx) {
  comment(
      "// FastArrayLoad r%u, r%u, r%u",
      frRes.index(),
      frArr.index(),
      frIdx.index());
#if defined(HERMESVM_COMPRESSED_POINTERS) || defined(HERMESVM_BOXED_DOUBLES)
  syncAllFRTempExcept(frRes != frArr && frRes != frIdx ? frRes : FR());
  syncToFrame(frArr);
  freeAllFRTempExcept({});
  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frArr);
  movHWFromFR(HWReg::vecD(0), frIdx);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(SHRuntime *, SHLegacyValue *, double idx),
      _sh_fastarray_load);
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
#else
  asmjit::Label slowPathLab = newSlowPathLabel();
  // We allocate a temporary register to compute the address instead of using
  // the result register in case the result has a VecD allocated for it.
  HWReg hwTmpStorage = allocTempGpX();
  HWReg hwTmpSize = allocTempGpX();
  HWReg hwTmpIdxGpX = allocTempGpX();
  HWReg hwTmpIdxVecD = allocTempVecD();
  HWReg hwArr = getOrAllocFRInGpX(frArr, true);
  HWReg hwIdx = getOrAllocFRInVecD(frIdx, true);
  // Done allocating, free the temps so they can be reused for the result.
  freeReg(hwTmpStorage);
  freeReg(hwTmpSize);
  freeReg(hwTmpIdxGpX);
  freeReg(hwTmpIdxVecD);

  // x86-64: as in fastArrayLength, assert the facts the fast path reads the
  // raw bits under -- frArr as a pointer, and frIdx as a double, which is what
  // getOrAllocFRInVecD above has already loaded it as. Both must precede the
  // comparison below, since emitTypeAssert clobbers EFLAGS.
  emitTypeAssert(frArr, hwArr, TypePred::IsObject);
  emitTypeAssert(frIdx, hwIdx, TypePred::IsNumber);

  // We will have to sync registers when the access is inside a try region
  // because we could read from the FRs again in this function.
  //
  // x86-64: arm64 syncs between the bounds comparison and the branch that
  // consumes it, which is safe there because its stores do not write the
  // condition flags. Nothing guarantees that for the moves this emits on x86,
  // so the sync is hoisted above the whole flag-producing sequence instead.
  // Syncing earlier is otherwise immaterial: it only writes registers back to
  // their canonical locations and frees nothing, so hwArr, hwIdx and the temps
  // above stay valid across it.
  //
  // isInTry() is unreachable at runtime today: it is only true inside a try
  // region, and any function with an exception table still declines in
  // leave() (its emitter contains a Catch instruction, which declines). This
  // is a deliberate sync-WITHOUT-free, kept in sync with the same call made
  // for the same reason in arm64/JitEmitter-control.cpp:113-120
  // (throwIfThisInitialized's isInTry() sync, citing this function's #else
  // path as its own precedent).
  if (isInTry())
    syncAllFRTempExcept(frRes != frArr && frRes != frIdx ? frRes : FR());

  // Retrieve the FastArray pointer and use it to load the indexed storage
  // pointer.
  emit_sh_ljs_get_pointer(a, hwTmpStorage.gpq(), hwArr.gpq());
  movHWFromMem(
      hwTmpStorage,
      x86::qword_ptr(
          hwTmpStorage.gpq(), offsetof(SHFastArray, indexedStorage)));

  // Load the size from the indexed storage.
  a.mov(
      hwTmpSize.gpq().r32(),
      x86::dword_ptr(hwTmpStorage.gpq(), offsetof(SHArrayStorageSmall, size)));

  // Check if the index is a uint32.
  emit_double_is_uint32(a, hwTmpIdxGpX.gpq(), hwTmpIdxVecD.xmm(), hwIdx.xmm());
  // x86-64: arm64 folds the failure of that check into the bounds comparison
  // with a ccmp, which x86 has no equivalent of, so the three ways to be
  // out-of-bounds are three separate branches to the same label: the index is
  // not an exact uint32, the index is a NaN (which vucomisd reports as equal
  // -- see emit_double_is_uint32), or it is not below the size.
  a.jne(slowPathLab);
  a.jp(slowPathLab);
  a.cmp(hwTmpSize.gpq().r32(), hwTmpIdxGpX.gpq().r32());
  a.jbe(slowPathLab);

  // x86-64: arm64 adds the offset of the data in the ArrayStorage into the
  // storage register and then uses a scaled register offset; on x86 the base,
  // the scaled index and that offset are all one memory operand, so nothing is
  // added up front. The index's upper 32 bits are zero (emit_double_is_uint32
  // leaves them so), which is what makes the 64-bit scaled index the uint32
  // one.
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false);
  movHWFromMem(
      hwRes,
      x86::qword_ptr(
          hwTmpStorage.gpq(),
          hwTmpIdxGpX.gpq(),
          3,
          offsetof(SHArrayStorageSmall, storage)));
  frUpdatedWithHW(frRes, hwRes);

  slowPaths_.emplace_back(
      slowPathLab,
      emittingIP,
      [frRes, frArr, frIdx](Emitter &em, SlowPath &sp) {
        em.comment(
            "// Slow path: FastArrayLoad r%u, r%u, r%u",
            frRes.index(),
            frArr.index(),
            frIdx.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        EMIT_RUNTIME_CALL(em, void (*)(SHRuntime *), _sh_throw_array_oob);
        // Call does not return.
      });
#endif
}

void Emitter::fastArrayStore(FR frArr, FR frIdx, FR frVal) {
  comment(
      "// FastArrayStore r%u, r%u, r%u",
      frArr.index(),
      frIdx.index(),
      frVal.index());
  syncAllFRTempExcept({});
  syncToFrame(frArr);
  syncToFrame(frVal);
  freeAllFRTempExcept({});
  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frVal);
  loadFrameAddr(x86::rdx, frArr);
  movHWFromFR(HWReg::vecD(0), frIdx);
  EMIT_RUNTIME_CALL(
      *this,
      void (*)(SHRuntime *, const SHLegacyValue *, SHLegacyValue *, double idx),
      _sh_fastarray_store);
}

void Emitter::fastArrayPush(FR frArr, FR frVal) {
  comment("// FastArrayPush r%u, r%u", frArr.index(), frVal.index());
  syncAllFRTempExcept({});
  syncToFrame(frArr);
  syncToFrame(frVal);
  freeAllFRTempExcept({});
  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frVal);
  loadFrameAddr(x86::rdx, frArr);
  EMIT_RUNTIME_CALL(
      *this,
      void (*)(SHRuntime *, SHLegacyValue *, SHLegacyValue *),
      _sh_fastarray_push);
}

void Emitter::fastArrayAppend(FR frArr, FR frOther) {
  comment("// FastArrayAppend r%u, r%u", frArr.index(), frOther.index());
  syncAllFRTempExcept({});
  syncToFrame(frArr);
  syncToFrame(frOther);
  freeAllFRTempExcept({});
  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frOther);
  loadFrameAddr(x86::rdx, frArr);
  EMIT_RUNTIME_CALL(
      *this,
      void (*)(SHRuntime *, SHLegacyValue *, SHLegacyValue *),
      _sh_fastarray_append);
}

} // namespace hermes::vm::x86_64
#endif // HERMESVM_JIT_X86_64

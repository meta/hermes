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
  // isInTry() is live since the exceptions milestone: a function with an
  // exception table now compiles, so a fast array load inside a try region
  // reaches this. See fastarrays.js's `tryLoad`, which loads inside a try and
  // reads the array registers again from the catch handler.
  //
  // This is a deliberate sync-WITHOUT-free, kept in sync with the same call
  // made for the same reason in JitEmitter-control.cpp
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

void Emitter::toPropertyKey(FR frRes, FR frVal) {
  comment("// ToPropertyKey r%u, r%u", frRes.index(), frVal.index());
  syncAllFRTempExcept(frRes != frVal ? frRes : FR());
  syncToFrame(frVal);
  freeAllFRTempExcept({});

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frVal);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(SHRuntime *, const SHLegacyValue *),
      _sh_ljs_to_property_key);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
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

void Emitter::getArgumentsLength(FR frRes, FR frLazyReg) {
  comment("// GetArgumentsLength r%u, r%u", frRes.index(), frLazyReg.index());

  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();

  syncAllFRTempExcept(frRes != frLazyReg ? frRes : FR());
  syncToFrame(frLazyReg);

  HWReg hwLazyReg = getOrAllocFRInGpX(frLazyReg, true);
  HWReg hwTemp = allocTempGpX();
  freeAllFRTempExcept({});
  freeReg(hwTemp);
  // Avoid an extra mov by using the temp register for the result if possible.
  HWReg hwRes = getOrAllocFRInVecD(frRes, false);
  frUpdatedWithHW(frRes, hwRes);

  // If the lazy register holds an object, `arguments` has already been
  // reified and the length has to be read off that object.
  emit_sh_ljs_is_object(a, hwTemp.gpq(), hwLazyReg.gpq());
  a.je(slowPathLab);

  // Fast path: if it's not an object, read from the frame.
  static_assert(
      HERMESVALUE_VERSION == 2,
      "NativeUint32 is stored as the lower 32 bits of the raw HermesValue");
  // x86-64: arm64 needs ldur for the negative frame offset; every x86 memory
  // operand carries a signed 32-bit displacement, so this is a plain mov.
  // The 32-bit load zero-extends, which is what makes the unsigned
  // conversion below exact.
  a.mov(
      hwTemp.gpq().r32(),
      x86::dword_ptr(
          xFrame,
          (int)StackFrameLayout::ArgCount * (int)sizeof(SHLegacyValue)));

  // Encode the uint32_t as a double (making it a HermesValue).
  // x86-64: stands in for arm64's ucvtf of a W register; see
  // emit_int32_to_double(), which clobbers hwTemp -- dead here.
  emit_int32_to_double(a, hwRes.xmm(), hwTemp.gpq(), /* isUnsigned */ true);

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [frRes, frLazyReg, hwRes](Emitter &em, SlowPath &sp) {
        em.comment(
            "// Slow path: GetArgumentsLength r%u, r%u",
            frRes.index(),
            frLazyReg.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        em.a.mov(x86::rsi, xFrame);
        em.loadFrameAddr(x86::rdx, frLazyReg);
        EMIT_RUNTIME_CALL(
            em,
            SHLegacyValue (*)(SHRuntime *, SHLegacyValue *, SHLegacyValue *),
            _sh_ljs_get_arguments_length);
        em.movHWFromHW<false>(hwRes, HWReg::gpX(0));
        em.a.jmp(sp.contLab);
      });
}

void Emitter::iteratorBegin(FR frRes, FR frSource) {
  comment("// IteratorBegin r%u, r%u", frRes.index(), frSource.index());

  syncAllFRTempExcept(frRes != frSource ? frRes : FR());
  syncToFrame(frSource);
  freeAllFRTempExcept({});

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frSource);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(SHRuntime *, SHLegacyValue *),
      _sh_ljs_iterator_begin_rjs);

  // frSource is an in/out parameter: the handler overwrites it with the
  // iteration index for the fast-array path, or leaves the iterator there.
  syncFrameOutParam(frSource);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::iteratorNext(FR frRes, FR frIteratorOrIdx, FR frSourceOrNext) {
  comment(
      "// IteratorNext r%u, r%u, r%u",
      frRes.index(),
      frIteratorOrIdx.index(),
      frSourceOrNext.index());

  syncAllFRTempExcept(
      frRes != frIteratorOrIdx && frRes != frSourceOrNext ? frRes : FR());
  syncToFrame(frIteratorOrIdx);
  syncToFrame(frSourceOrNext);
  freeAllFRTempExcept({});

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frIteratorOrIdx);
  loadFrameAddr(x86::rdx, frSourceOrNext);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(SHRuntime *, SHLegacyValue *, const SHLegacyValue *),
      _sh_ljs_iterator_next_rjs);

  // The index is bumped in place on the fast-array path.
  syncFrameOutParam(frIteratorOrIdx);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::iteratorClose(FR frIteratorOrIdx, bool ignoreExceptions) {
  comment(
      "// IteratorClose r%u, %u", frIteratorOrIdx.index(), ignoreExceptions);

  syncAllFRTempExcept({});
  syncToFrame(frIteratorOrIdx);
  freeAllFRTempExcept({});

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frIteratorOrIdx);
  a.mov(x86::edx, asmjit::Imm(ignoreExceptions));
  EMIT_RUNTIME_CALL(
      *this,
      void (*)(SHRuntime *, const SHLegacyValue *, bool),
      _sh_ljs_iterator_close_rjs);
}

void Emitter::getPNameList(FR frRes, FR frObj, FR frIdx, FR frSize) {
  comment(
      "// GetPNameList r%u, r%u, r%u, r%u",
      frRes.index(),
      frObj.index(),
      frIdx.index(),
      frSize.index());
  syncAllFRTempExcept({});
  // We have to sync frObj to the frame since it is an in/out parameter.
  syncToFrame(frObj);
  // No need to sync frIdx and frSize since they are just out parameters.
  freeAllFRTempExcept({});
  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frObj);
  loadFrameAddr(x86::rdx, frIdx);
  loadFrameAddr(x86::rcx, frSize);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(
          SHRuntime *, SHLegacyValue *, SHLegacyValue *, SHLegacyValue *),
      _sh_ljs_get_pname_list_rjs);

  // Ensure that the out params have their frame location marked as up-to-date,
  // and any global register is updated.
  syncFrameOutParam(frObj);
  syncFrameOutParam(frIdx);
  syncFrameOutParam(frSize);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::getNextPName(
    FR frRes,
    FR frProps,
    FR frObj,
    FR frIdx,
    FR frSize) {
  comment(
      "// GetNextPName r%u, r%u, r%u, r%u, r%u",
      frRes.index(),
      frProps.index(),
      frObj.index(),
      frIdx.index(),
      frSize.index());

  syncAllFRTempExcept({});
  syncToFrame(frProps);
  syncToFrame(frObj);
  syncToFrame(frIdx);
  syncToFrame(frSize);
  freeAllFRTempExcept({});
  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frProps);
  loadFrameAddr(x86::rdx, frObj);
  loadFrameAddr(x86::rcx, frIdx);
  loadFrameAddr(x86::r8, frSize);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(
          SHRuntime *,
          SHLegacyValue *,
          SHLegacyValue *,
          SHLegacyValue *,
          SHLegacyValue *),
      _sh_ljs_get_next_pname_rjs);

  // Ensure that the updated frame location is sync'd back to any global reg.
  syncFrameOutParam(frIdx);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::getArgumentsPropByValImpl(
    FR frRes,
    FR frIndex,
    FR frLazyReg,
    const char *name,
    SHLegacyValue (*shImpl)(
        SHRuntime *shr,
        SHLegacyValue *frame,
        SHLegacyValue *idx,
        SHLegacyValue *lazyReg),
    const char *shImplName) {
  comment(
      "// %s r%u, r%u, r%u",
      name,
      frRes.index(),
      frIndex.index(),
      frLazyReg.index());

  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();

  syncAllFRTempExcept(frRes != frIndex && frRes != frLazyReg ? frRes : FR());
  syncToFrame(frIndex);
  syncToFrame(frLazyReg);
  HWReg hwLazyReg = getOrAllocFRInGpX(frLazyReg, true);
  HWReg hwIndex = getOrAllocFRInVecD(frIndex, true);
  HWReg hwTempIndex = allocTempGpX();
  HWReg hwTempArgCount = allocTempGpX();
  HWReg hwTempVecD = allocTempVecD();
  freeAllFRTempExcept({});
  freeReg(hwTempIndex);
  freeReg(hwTempArgCount);
  freeReg(hwTempVecD);
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);

  // If lazyReg is an object, `arguments` has been reified and the read has to
  // go through the object; go to slow path.
  emit_sh_ljs_is_object(a, hwTempIndex.gpq(), hwLazyReg.gpq());
  a.je(slowPathLab);

  // If index is not an array index, go to slow path.
  emit_double_is_int(a, hwTempIndex.gpq(), hwTempVecD.xmm(), hwIndex.xmm());
  // x86-64: arm64's single b.ne. vucomisd reports an unordered compare as
  // EQUAL, so a NaN index would fall through into the bounds check with the
  // converted value INT64_MIN -- which the unsigned compare below happens to
  // reject anyway, but only by accident. The jp states the intent and keeps
  // this consistent with every other emit_double_is_* caller; see that
  // helper's contract.
  a.jne(slowPathLab);
  a.jp(slowPathLab);

  // If index >= arg count or index < 0, go to slow path.
  // Use an unsigned comparison to handle the negative index case: the
  // conversion above sign-extends, so a negative index is a huge unsigned.
  a.mov(
      hwTempArgCount.gpq().r32(),
      x86::dword_ptr(
          xFrame,
          (int)StackFrameLayout::ArgCount * (int)sizeof(SHLegacyValue)));
  a.cmp(hwTempIndex.gpq(), hwTempArgCount.gpq());
  a.jae(slowPathLab);

  // Load the argument from the stack. We want framePtr[FirstArg - index].
  //
  // x86-64: arm64 computes (FirstArg - index) into a register and uses a
  // scaled register offset with an SXTW. x86 addressing has no negative
  // scale, so the index is negated instead and FirstArg's byte offset becomes
  // the operand's displacement: xFrame + (-index)*8 + FirstArg*8. The negate
  // is exact in 64 bits because the comparison above has already established
  // 0 <= index < argCount, i.e. that the value fits in 32 bits.
  a.neg(hwTempIndex.gpq());
  movHWFromMem(
      hwRes,
      x86::qword_ptr(
          xFrame,
          hwTempIndex.gpq(),
          3,
          (int)StackFrameLayout::FirstArg * (int)sizeof(SHLegacyValue)));

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [name, frIndex, frLazyReg, hwRes, shImpl, shImplName](
          Emitter &em, SlowPath &sp) {
        em.comment("// Slow path: %s r%u", name, frIndex.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        em.a.mov(x86::rsi, xFrame);
        em.loadFrameAddr(x86::rdx, frIndex);
        em.loadFrameAddr(x86::rcx, frLazyReg);
        em.callRuntimeWithSavedIP((void *)shImpl, shImplName);
        em.movHWFromHW<false>(hwRes, HWReg::gpX(0));
        em.a.jmp(sp.contLab);
      });
}

void Emitter::reifyArgumentsImpl(FR frLazyReg, bool strict, const char *name) {
  comment("// %s r%u", name, frLazyReg.index());

  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();

  syncAllFRTempExcept({});
  syncToFrame(frLazyReg);

  HWReg hwLazyReg = getOrAllocFRInGpX(frLazyReg, true);
  HWReg hwTemp = allocTempGpX();
  freeAllFRTempExcept({});
  freeReg(hwTemp);

  emit_sh_ljs_is_object(a, hwTemp.gpq(), hwLazyReg.gpq());
  // If the lazyReg is not an object, it needs to be reified, go to slow path.
  a.jne(slowPathLab);

  // Fast path: do nothing.
  a.bind(contLab);

  // x86-64 DELTA, and not a cosmetic one. frLazyReg is an in/out operand:
  // whichever path was taken, it holds the Arguments OBJECT from here on.
  // Its recorded type usually says otherwise -- ISel initializes the lazy
  // register with LoadConstUndefined, whose emitter records
  // FRType::OtherNonPtr, and neither arm64's version of this function nor
  // this one writes the register in emitted code, so nothing else clears
  // that. A stale "non-pointer" claim then propagates through Mov and licenses
  // fast paths that are only valid for non-pointers -- strictEqualImpl's
  // raw-bit tier is the one that showed up. This backend emits type asserts
  // (-Xjit-emit-type-asserts), which caught it: args.js's `reifyTwice` fails
  // the BitComparable assert without the line below. Widen the type to
  // "unknown" instead, which is true on both paths.
  //
  // arm64 STILL CARRIES THIS HOLE and is deliberately left unfixed here: the
  // milestone contract for this branch is that arm64's emitted code stays
  // byte-identical to its baseline, and this line changes the emitter's
  // type state. arm64 has no test that flows a reified `arguments` into a
  // type-sensitive emitter, which is why it has never been caught there.
  // Fixing it is a separate change against arm64, not this one.
  frUpdateType(frLazyReg, FRType::UnknownPtr);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [name,
       frLazyReg,
       strict,
       hwGlobalReg = frameRegs_[frLazyReg.index()].globalReg](
          Emitter &em, SlowPath &sp) {
        em.comment("// Slow path: %s r%u", name, frLazyReg.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        em.a.mov(x86::rsi, xFrame);
        em.loadFrameAddr(x86::rdx, frLazyReg);
        em.callRuntimeWithSavedIP(
            strict ? (void *)_sh_ljs_reify_arguments_strict
                   : (void *)_sh_ljs_reify_arguments_loose,
            strict ? "_sh_ljs_reify_arguments_strict"
                   : "_sh_ljs_reify_arguments_loose");
        // Slow path modifies the frame so we need to sync it if there's a
        // global reg. This branch is dead on x86-64: a lazy-arguments FR can
        // never be assigned Number/NonPtr class, so a global register is
        // never allocated for it and hwGlobalReg.isValid() is always false.
        // It is kept only for textual parity with arm64, where the same
        // branch is inherited unchanged; if it ever did run here it would
        // bypass recordFRWriteForAssert.
        assert(
            !hwGlobalReg.isValid() &&
            "lazy arguments FR can never have a global register");
        if (hwGlobalReg.isValid()) {
          em._loadFrame(hwGlobalReg, frLazyReg);
        }
        em.a.jmp(sp.contLab);
      });
}

} // namespace hermes::vm::x86_64
#endif // HERMESVM_JIT_X86_64

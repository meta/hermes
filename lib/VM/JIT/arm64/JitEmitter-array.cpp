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

namespace hermes::vm::arm64 {

void Emitter::newArray(FR frRes, uint32_t size) {
  comment("// NewArray r%u, %u", frRes.index(), size);
  syncAllFRTempExcept(frRes);
  freeAllFRTempExcept({});
  a.mov(a64::x0, xRuntime);
  a.mov(a64::w1, size);
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

  syncAllFRTempExcept(frRes);
  freeAllFRTempExcept({});
  a.mov(a64::x0, xRuntime);
  loadBits64InGp(a64::x1, (uint64_t)codeBlock_, "CodeBlock");
  a.mov(a64::w2, numElements);
  a.mov(a64::w3, numLiterals);
  a.mov(a64::w4, bufferIndex);
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
  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frProto);
  a.mov(a64::w2, size);
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
  emit_sh_ljs_get_pointer(a, temp.a64GpX(), hwArr.a64GpX());

#ifdef HERMESVM_BOXED_DOUBLES
  // If boxed doubles are enabled, load the size from the ArrayStorage, where it
  // is stored as an integer.
  emit_load_cp(
      a,
      temp.a64GpX(),
      a64::Mem(temp.a64GpX(), offsetof(SHFastArray, indexedStorage)));
  emit_sh_cp_decode_non_null(a, temp.a64GpX());
  a.ldr(
      temp.a64GpX().w(),
      a64::Mem(temp.a64GpX(), offsetof(SHArrayStorage, size)));
  HWReg hwRes = getOrAllocFRInVecD(frRes, false);
  a.ucvtf(hwRes.a64VecD(), temp.a64GpX().w());
#else
  // If boxed doubles are disabled, we can just load the size from the length
  // property of the FastArray.
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false);
  movHWFromMem(hwRes, a64::Mem(temp.a64GpX(), offsetof(SHFastArray, length)));
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
  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frArr);
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

  // Retrieve the FastArray pointer and use it to load the indexed storage
  // pointer.
  emit_sh_ljs_get_pointer(a, hwTmpStorage.a64GpX(), hwArr.a64GpX());
  movHWFromMem(
      hwTmpStorage,
      a64::Mem(hwTmpStorage.a64GpX(), offsetof(SHFastArray, indexedStorage)));

  // Load the size from the indexed storage.
  a.ldr(
      hwTmpSize.a64GpX().w(),
      a64::Mem(hwTmpStorage.a64GpX(), offsetof(SHArrayStorageSmall, size)));

  // Check if the index is a uint32.
  emit_double_is_uint32(
      a, hwTmpIdxGpX.a64GpX().w(), hwTmpIdxVecD.a64VecD(), hwIdx.a64VecD());
  // If the conversion was successful, compare the size against the index.
  // Otherwise, set the flags to zero to force the subsequent b_ls to be taken.
  a.ccmp(
      hwTmpSize.a64GpX().w(), hwTmpIdxGpX.a64GpX().w(), 0, a64::CondCode::kEQ);
  // If the index is out-of-bounds jump to the failure path.
  // We will have to sync registers when the access is inside a try region
  // because we could read from the FRs again in this function.
  if (isInTry())
    syncAllFRTempExcept(frRes != frArr && frRes != frIdx ? frRes : FR());
  a.b_ls(slowPathLab);

  // Add the offset of the actual data in the ArrayStorage.
  a.add(
      hwTmpStorage.a64GpX(),
      hwTmpStorage.a64GpX(),
      offsetof(SHArrayStorageSmall, storage));

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false);
  movHWFromMem(
      hwRes,
      a64::Mem(hwTmpStorage.a64GpX(), hwTmpIdxGpX.a64GpX(), a64::lsl(3)));
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
        em.a.mov(a64::x0, xRuntime);
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
  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frVal);
  loadFrameAddr(a64::x2, frArr);
  movHWFromFR(HWReg{a64::d0}, frIdx);
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
  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frVal);
  loadFrameAddr(a64::x2, frArr);
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
  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frOther);
  loadFrameAddr(a64::x2, frArr);
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

  emit_sh_ljs_is_object(a, hwTemp.a64GpX(), hwLazyReg.a64GpX());
  a.b_eq(slowPathLab);

  // Fast path: if it's not an object, read from the frame.
  static_assert(
      HERMESVALUE_VERSION == 2,
      "NativeUint32 is stored as the lower 32 bits of the raw HermesValue");
  a.ldur(
      hwTemp.a64GpX().w(),
      a64::Mem(
          xFrame,
          (int)StackFrameLayout::ArgCount * (int)sizeof(SHLegacyValue)));

  // Encode the uint32_t as a double (making it a HermesValue).
  a.ucvtf(hwRes.a64VecD(), hwTemp.a64GpX().w());

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
        em.a.mov(a64::x0, xRuntime);
        em.a.mov(a64::x1, xFrame);
        em.loadFrameAddr(a64::x2, frLazyReg);
        EMIT_RUNTIME_CALL(
            em,
            SHLegacyValue (*)(SHRuntime *, SHLegacyValue *, SHLegacyValue *),
            _sh_ljs_get_arguments_length);
        em.movHWFromHW<false>(hwRes, HWReg::gpX(0));
        em.a.b(sp.contLab);
      });
}

void Emitter::iteratorBegin(FR frRes, FR frSource) {
  comment("// IteratorBegin r%u, r%u", frRes.index(), frSource.index());

  syncAllFRTempExcept(frRes != frSource ? frRes : FR());
  syncToFrame(frSource);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frSource);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(SHRuntime *, SHLegacyValue *),
      _sh_ljs_iterator_begin_rjs);

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

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frIteratorOrIdx);
  loadFrameAddr(a64::x2, frSourceOrNext);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(SHRuntime *, SHLegacyValue *, const SHLegacyValue *),
      _sh_ljs_iterator_next_rjs);

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

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frIteratorOrIdx);
  a.mov(a64::w2, ignoreExceptions);
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
  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frObj);
  loadFrameAddr(a64::x2, frIdx);
  loadFrameAddr(a64::x3, frSize);
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
  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frProps);
  loadFrameAddr(a64::x2, frObj);
  loadFrameAddr(a64::x3, frIdx);
  loadFrameAddr(a64::x4, frSize);
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

void Emitter::toPropertyKey(FR frRes, FR frVal) {
  comment("// ToPropertyKey r%u, r%u", frRes.index(), frVal.index());
  syncAllFRTempExcept(frRes != frVal ? frRes : FR());
  syncToFrame(frVal);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
  loadFrameAddr(a64::x1, frVal);
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(SHRuntime *, const SHLegacyValue *),
      _sh_ljs_to_property_key);

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
  a64::GpW wTempIndex = hwTempIndex.a64GpX().w();
  HWReg hwTempArgCount = allocTempGpX();
  HWReg hwTempVecD = allocTempVecD();
  freeAllFRTempExcept({});
  freeReg(hwTempIndex);
  freeReg(hwTempArgCount);
  freeReg(hwTempVecD);
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);

  // If lazyReg is an object, go to slow path.
  emit_sh_ljs_is_object(a, hwTempIndex.a64GpX(), hwLazyReg.a64GpX());
  a.b_eq(slowPathLab);

  // If index is not an array index, go to slow path.
  emit_double_is_int(
      a, hwTempIndex.a64GpX(), hwTempVecD.a64VecD(), hwIndex.a64VecD());
  a.b_ne(slowPathLab);

  // If index >= arg count or index < 0, go to slow path.
  // Use an unsigned comparison to handle the negative index case.
  a.ldur(
      hwTempArgCount.a64GpX().w(),
      a64::Mem(
          xFrame,
          (int)StackFrameLayout::ArgCount * (int)sizeof(SHLegacyValue)));
  a.cmp(hwTempIndex.a64GpX(), hwTempArgCount.a64GpX());
  a.b_hs(slowPathLab);

  // Load the argument from the stack.
  // We want framePtr[(firstArg - index) * 8].
  // Use shift SXTW to shift the signed w register by 3.
  a.mov(hwTempArgCount.a64GpX().w(), (int)StackFrameLayout::FirstArg);
  a.sub(wTempIndex, hwTempArgCount.a64GpX().w(), wTempIndex);
  a.ldr(
      hwRes.a64GpX(),
      a64::Mem(xFrame, wTempIndex, a64::Shift(a64::ShiftOp::kSXTW, 3)));

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [name, frIndex, frLazyReg, hwRes, shImpl, shImplName](
          Emitter &em, SlowPath &sp) {
        em.comment("// Slow path: %s r%u", name, frIndex.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(a64::x0, xRuntime);
        em.a.mov(a64::x1, xFrame);
        em.loadFrameAddr(a64::x2, frIndex);
        em.loadFrameAddr(a64::x3, frLazyReg);
        em.callRuntimeWithSavedIP((void *)shImpl, shImplName);
        em.movHWFromHW<false>(hwRes, HWReg::gpX(0));
        em.a.b(sp.contLab);
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

  emit_sh_ljs_is_object(a, hwTemp.a64GpX(), hwLazyReg.a64GpX());
  // If the lazyReg is not an object, it needs to be reified, go to slow path.
  a.b_ne(slowPathLab);

  // Fast path: do nothing.
  a.bind(contLab);

  // frLazyReg is an in/out operand: whichever path was taken, it holds the
  // Arguments OBJECT from here on. Its recorded type usually says otherwise
  // -- ISel initializes the lazy register with LoadConstUndefined, whose
  // emitter records FRType::OtherNonPtr, and neither path above writes the
  // register in emitted code, so nothing else clears that. A stale
  // "non-pointer" claim then propagates through Mov and licenses fast paths
  // that are only valid for non-pointers -- strictEqualImpl's raw-bit tier
  // is the one that showed up. Widen the type to "unknown" instead, which
  // is true on both paths. Mirrors x86-64's fix in
  // JitEmitter-array.cpp's reifyArgumentsImpl.
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
        em.a.mov(a64::x0, xRuntime);
        em.a.mov(a64::x1, xFrame);
        em.loadFrameAddr(a64::x2, frLazyReg);
        em.callRuntimeWithSavedIP(
            strict ? (void *)_sh_ljs_reify_arguments_strict
                   : (void *)_sh_ljs_reify_arguments_loose,
            strict ? "_sh_ljs_reify_arguments_strict"
                   : "_sh_ljs_reify_arguments_loose");
        // Slow path modifies the frame so we need to sync it if there's a
        // global reg.
        if (hwGlobalReg.isValid()) {
          em._loadFrame(hwGlobalReg, frLazyReg);
        }
        em.a.b(sp.contLab);
      });
}

} // namespace hermes::vm::arm64
#endif // HERMESVM_JIT_ARM64

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JIT/Config.h"
#if HERMESVM_JIT_X86_64
#include "JitEmitter-internal.h"
#include "JitEmitter.h"

#include "../JitHandlers.h"

#include "../RuntimeOffsets.h"

namespace hermes::vm::x86_64 {

void Emitter::toNumber(FR frRes, FR frInput) {
  comment("// %s r%u, r%u", "toNumber", frRes.index(), frInput.index());
  if (isFRKnownNumber(frInput)) {
    emitTypeAssertFR(frInput, TypePred::IsNumber);
    return mov(frRes, frInput, false);
  }

  HWReg hwRes, hwInput;
  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();
  syncAllFRTempExcept(frRes != frInput ? frRes : FR());
  syncToFrame(frInput);

  hwInput = getOrAllocFRInVecD(frInput, true);

  // We don't free frRes so that if it is the same as frThis, the register is
  // simply persisted and we do not need to perform a move in the fast path.
  freeAllFRTempExcept(frRes);
  hwRes = getOrAllocFRInVecD(frRes, false);
  frUpdatedWithHW(frRes, hwRes, FRType::Number);

  // Since HermesValue is NaN-boxed we know that all non-number values will be
  // NaN. So we can conveniently test for non-number values by checking for NaN
  // (which does not compare equal to itself).
  // x86-64: vucomisd reports unordered in PF, and sets ZF as well, so
  // arm64's `b.ne` has no equivalent here -- the NaN test is `jp`.
  static_assert(HERMESVALUE_VERSION == 2, "Non-numbers must be NaN");
  a.vucomisd(hwInput.xmm(), hwInput.xmm());
  a.jp(slowPathLab);
  movHWFromHW<false>(hwRes, hwInput);

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [frRes, frInput, hwRes](Emitter &em, SlowPath &sp) {
        em.comment(
            "// Slow path: toNumber r%u, r%u", frRes.index(), frInput.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        em.loadFrameAddr(x86::rsi, frInput);
        EMIT_RUNTIME_CALL(
            em,
            double (*)(SHRuntime *, const SHLegacyValue *),
            _sh_ljs_to_double_rjs);
        em.movHWFromHW<false>(hwRes, HWReg::vecD(0));
        em.a.jmp(sp.contLab);
      });
}

void Emitter::toNumeric(FR frRes, FR frInput) {
  comment("// %s r%u, r%u", "toNumeric", frRes.index(), frInput.index());
  if (isFRKnownNumber(frInput)) {
    emitTypeAssertFR(frInput, TypePred::IsNumber);
    return mov(frRes, frInput, false);
  }

  HWReg hwRes, hwInput;
  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();
  syncAllFRTempExcept(frRes != frInput ? frRes : FR());
  syncToFrame(frInput);

  hwInput = getOrAllocFRInVecD(frInput, true);

  // We don't free frRes so that if it is the same as frThis, the register is
  // simply persisted and we do not need to perform a move in the fast path.
  freeAllFRTempExcept(frRes);
  hwRes = getOrAllocFRInVecD(frRes, false);
  frUpdatedWithHW(frRes, hwRes, FRType::UnknownPtr);

  // Since HermesValue is NaN-boxed we know that all non-number values will be
  // NaN. So we can conveniently test for non-number values by checking for NaN
  // (which does not compare equal to itself).
  // x86-64: see toNumber -- the unordered result lives in PF, so `jp`.
  static_assert(HERMESVALUE_VERSION == 2, "Non-numbers must be NaN");
  a.vucomisd(hwInput.xmm(), hwInput.xmm());
  a.jp(slowPathLab);
  movHWFromHW<false>(hwRes, hwInput);

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [frRes, frInput, hwRes](Emitter &em, SlowPath &sp) {
        em.comment(
            "// Slow path: toNumeric r%u, r%u", frRes.index(), frInput.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        em.loadFrameAddr(x86::rsi, frInput);
        EMIT_RUNTIME_CALL(
            em,
            SHLegacyValue (*)(SHRuntime *, const SHLegacyValue *),
            _sh_ljs_to_numeric_rjs);
        // x86-64: a one-eightbyte struct whose union mixes an integer and a
        // double classifies as INTEGER under SysV, so SHLegacyValue comes
        // back in rax, exactly as it comes back in x0 on arm64.
        em.movHWFromHW<false>(hwRes, HWReg::gpX(0));
        em.a.jmp(sp.contLab);
      });
}

void Emitter::toInt32(FR frRes, FR frInput, bool isSigned) {
  comment(
      "// %s r%u, r%u",
      isSigned ? "ToInt32" : "ToUint32",
      frRes.index(),
      frInput.index());

  HWReg hwTempGpX = allocTempGpX();
  HWReg hwTempVecD = allocTempVecD();

  syncAllFRTempExcept(frRes != frInput ? frRes : FR());
  // TODO: As with binary bit ops, it should be possible to only do this in the
  // slow path.
  syncToFrame(frInput);

  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();

  HWReg hwInput = getOrAllocFRInVecD(frInput, true);
  emit_double_is_int(a, hwTempGpX.gpq(), hwTempVecD.xmm(), hwInput.xmm());
  // x86-64: two branches where arm64 has one. See emit_double_is_int: an
  // ordered mismatch shows up in ZF, an unordered compare (the input is a
  // NaN, so any non-number, or the JS NaN) only in PF.
  a.jne(slowPathLab);
  a.jp(slowPathLab);

  // Done allocating registers. Free them all and allocate the result.
  freeAllFRTempExcept({});
  freeReg(hwTempGpX);
  freeReg(hwTempVecD);
  HWReg hwRes = getOrAllocFRInVecD(frRes, false);
  frUpdatedWithHW(frRes, hwRes, FRType::Number);

  // Convert the int32 back to a double.
  emit_int32_to_double(
      a, hwRes.xmm(), hwTempGpX.gpq(), /* isUnsigned */ !isSigned);

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [isSigned, frRes, frInput, hwRes](Emitter &em, SlowPath &sp) {
        em.comment(
            "// Slow path: %s, r%u, r%u",
            isSigned ? "ToInt32" : "ToUint32",
            frRes.index(),
            frInput.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        em.loadFrameAddr(x86::rsi, frInput);
        em.callRuntimeWithSavedIP(
            isSigned ? (void *)_sh_ljs_to_int32_rjs
                     : (void *)_sh_ljs_to_uint32_rjs,
            isSigned ? "_sh_ljs_to_int32_rjs" : "_sh_ljs_to_uint32_rjs");
        // x86-64: these return a double, so the result arrives in xmm0,
        // where arm64 reads it out of d0.
        em.movHWFromHW<false>(hwRes, HWReg::vecD(0));
        em.a.jmp(sp.contLab);
      });
}

void Emitter::addEmptyString(FR frRes, FR frInput) {
  comment("// AddEmptyString r%u, r%u", frRes.index(), frInput.index());

  syncAllFRTempExcept(frRes != frInput ? frRes : FR());
  // TODO: As with binary bit ops, it should be possible to only do this in the
  // slow path.
  syncToFrame(frInput);

  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();

  HWReg hwInput = getOrAllocFRInGpX(frInput, true);
  HWReg hwTemp = allocTempGpX();
  freeReg(hwTemp);
  freeAllFRTempExcept(frRes);

  HWReg hwRes = getOrAllocFRInGpX(frRes, false);

  // Check if the input is already a string and don't do anything.
  // x86-64: emit_sh_ljs_is_string() leaves its answer in EFLAGS and this jne
  // is what consumes it, so nothing that writes flags may be placed between
  // the two. Any reorder here is a bug.
  emit_sh_ljs_is_string(a, hwTemp.gpq(), hwInput.gpq());
  a.jne(slowPathLab);

  // Fast path.
  movHWFromHW<false>(hwRes, hwInput);
  frUpdatedWithHW(frRes, hwRes, FRType::Pointer);

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [frRes, frInput, hwRes](Emitter &em, SlowPath &sp) {
        em.comment(
            "// Slow path: AddEmptyString r%u, r%u",
            frRes.index(),
            frInput.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        em.loadFrameAddr(x86::rsi, frInput);
        EMIT_RUNTIME_CALL(
            em,
            SHLegacyValue (*)(SHRuntime *, const SHLegacyValue *),
            _sh_ljs_add_empty_string_rjs);
        em.movHWFromHW<false>(hwRes, HWReg::gpX(0));
        em.a.jmp(sp.contLab);
      });
}

void Emitter::arithUnop(
    bool forceNumber,
    FR frRes,
    FR frInput,
    const char *name,
    void (*fast)(
        Emitter &em,
        const x86::Xmm &d,
        const x86::Xmm &s,
        const x86::Xmm &tmp),
    void *slowCall,
    const char *slowCallName) {
  comment("// %s r%u, r%u", name, frRes.index(), frInput.index());

  HWReg hwRes, hwInput;
  asmjit::Label slowPathLab;
  asmjit::Label contLab;
  bool inputIsNum;

  if (forceNumber) {
    frameRegs_[frInput.index()].localType = FRType::Number;
    inputIsNum = true;
  } else {
    inputIsNum = isFRKnownNumber(frInput);
  }

  hwInput = getOrAllocFRInVecD(frInput, true);
  if (inputIsNum)
    emitTypeAssert(frInput, hwInput, TypePred::IsNumber);
  if (!inputIsNum) {
    slowPathLab = newSlowPathLabel();
    contLab = newContLabel();
    syncAllFRTempExcept(frRes != frInput ? frRes : FR());
    syncToFrame(frInput);

    // Since HermesValue is NaN-boxed we know that all non-number values will be
    // NaN. So we can conveniently test for non-number values by checking for
    // NaN (which does not compare equal to itself).
    // x86-64: unordered lands in PF, so the NaN test is `jp`.
    static_assert(HERMESVALUE_VERSION == 2, "Non-numbers must be NaN");
    a.vucomisd(hwInput.xmm(), hwInput.xmm());
    a.jp(slowPathLab);
  }

  hwRes = getOrAllocFRInVecD(frRes, false);
  HWReg hwTmp = hwRes != hwInput ? hwRes : allocTempVecD();
  fast(*this, hwRes.xmm(), hwInput.xmm(), hwTmp.xmm());
  if (hwRes == hwInput)
    freeReg(hwTmp);

  frUpdatedWithHW(
      frRes, hwRes, inputIsNum ? FRType::Number : FRType::UnknownPtr);

  if (inputIsNum)
    return;

  freeAllFRTempExcept(frRes);
  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [name, frRes, frInput, hwRes, slowCall, slowCallName](
          Emitter &em, SlowPath &sp) {
        em.comment(
            "// Slow path: %s r%u, r%u", name, frRes.index(), frInput.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        em.loadFrameAddr(x86::rsi, frInput);
        em.callRuntimeWithSavedIP(slowCall, slowCallName);
        em.movHWFromHW<false>(hwRes, HWReg::gpX(0));
        em.a.jmp(sp.contLab);
      });
}

void Emitter::booleanNot(FR frRes, FR frInput) {
  comment("// Not r%u, r%u", frRes.index(), frInput.index());

  // TODO: Add a fast path, perhaps by sharing some code with JmpTrue.
  // x86-64: the by-value SHLegacyValue argument goes in rdi, not in the
  // first allocatable Gp (arm64's x0 is both).
  syncAndFreeTempReg(HWReg(x86::rdi));
  movHWFromFR(HWReg(x86::rdi), frInput);

  // Since we already loaded the input, no need to check for frRes == frInput.
  syncAllFRTempExcept(frRes);
  freeAllFRTempExcept({});
  EMIT_RUNTIME_CALL(*this, bool (*)(SHLegacyValue), _sh_ljs_to_boolean);

  HWReg hwRes = getOrAllocFRInGpX(frRes, false);
  // x86-64: SysV leaves everything above al undefined for a bool return, so
  // the value has to be zero-extended before it can be negated and encoded
  // (arm64 gets a clean 0/1 in x0).
  a.movzx(hwRes.gpq().r32(), x86::al);
  // Negate the result.
  a.xor_(hwRes.gpq().r32(), asmjit::Imm(1));
  // Add the bool tag.
  HWReg hwTmp = allocTempGpX();
  freeReg(hwTmp);
  emit_sh_ljs_bool(a, hwRes.gpq(), hwTmp.gpq());
  frUpdatedWithHW(frRes, hwRes, FRType::Bool);
}

void Emitter::bitNot(FR frRes, FR frInput) {
  comment("// BitNot r%u, r%u", frRes.index(), frInput.index());

  HWReg hwTempGpX = allocTempGpX();
  HWReg hwTempVecD = allocTempVecD();

  syncAllFRTempExcept(frRes != frInput ? frRes : FR());
  // TODO: As with binary bit ops, it should be possible to only do this in the
  // slow path.
  syncToFrame(frInput);

  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();

  HWReg hwInput = getOrAllocFRInVecD(frInput, true);
  emit_double_is_int(a, hwTempGpX.gpq(), hwTempVecD.xmm(), hwInput.xmm());
  // x86-64: see toInt32 -- the unordered case needs its own branch.
  a.jne(slowPathLab);
  a.jp(slowPathLab);

  // Done allocating registers. Free them all and allocate the result.
  freeAllFRTempExcept({});
  freeReg(hwTempGpX);
  freeReg(hwTempVecD);
  HWReg hwRes = getOrAllocFRInVecD(frRes, false);
  frUpdatedWithHW(
      frRes,
      hwRes,
      isFRKnownType(frInput, FRType::Number) ? FRType::Number
                                             : FRType::UnknownPtr);

  // Perform the negation and write it to the result.
  a.not_(hwTempGpX.gpq().r32());
  emit_int32_to_double(a, hwRes.xmm(), hwTempGpX.gpq(), /* isUnsigned */ false);

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [frRes, frInput, hwRes](Emitter &em, SlowPath &sp) {
        em.comment(
            "// Slow path: bitNot r%u, r%u", frRes.index(), frInput.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        em.loadFrameAddr(x86::rsi, frInput);
        EMIT_RUNTIME_CALL(
            em,
            SHLegacyValue (*)(SHRuntime *, const SHLegacyValue *),
            _sh_ljs_bit_not_rjs);
        em.movHWFromHW<false>(hwRes, HWReg::gpX(0));
        em.a.jmp(sp.contLab);
      });
}

void Emitter::typeOf(FR frRes, FR frInput) {
  comment("// TypeOf r%u, r%u", frRes.index(), frInput.index());
  syncAllFRTempExcept(frRes == frInput ? FR() : frRes);
  syncToFrame(frInput);
  freeAllFRTempExcept(FR());

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frInput);
  // TODO: Use a function that preserves temporary registers.
  EMIT_RUNTIME_CALL(
      *this, SHLegacyValue (*)(SHRuntime *, SHLegacyValue *), _sh_ljs_typeof);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::addS(FR frRes, FR frLeft, FR frRight) {
  comment(
      "// AddS r%u, r%u, r%u", frRes.index(), frLeft.index(), frRight.index());

  // x86-64: as on arm64 this is an unconditional runtime call. AddS is only
  // emitted when the compiler already proved both operands are strings, so
  // there is no type check to fast-path around; the work that is left --
  // allocating and filling the concatenation, or building a rope -- is all
  // the runtime's.
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
      _sh_ljs_string_add);
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::mod(bool forceNumber, FR frRes, FR frLeft, FR frRight) {
  comment(
      "// %s%s r%u, r%u, r%u",
      "mod",
      forceNumber ? "N" : "",
      frRes.index(),
      frLeft.index(),
      frRight.index());
  HWReg hwRes, hwLeft, hwRight;
  asmjit::Label slowPathLab;
  asmjit::Label contLab;
  bool leftIsNum, rightIsNum, slow;

  if (forceNumber) {
    frameRegs_[frLeft.index()].localType = FRType::Number;
    frameRegs_[frRight.index()].localType = FRType::Number;
    leftIsNum = rightIsNum = true;
    slow = false;
  } else {
    leftIsNum = isFRKnownNumber(frLeft);
    rightIsNum = isFRKnownNumber(frRight);
    slow = !(rightIsNum && leftIsNum);
  }

  syncAllFRTempExcept(frRes != frLeft && frRes != frRight ? frRes : FR());

  if (slow) {
    slowPathLab = newSlowPathLabel();
    contLab = newContLabel();
    syncToFrame(frLeft);
    syncToFrame(frRight);
  }

  hwLeft = getOrAllocFRInVecD(frLeft, true);
  hwRight = getOrAllocFRInVecD(frRight, true);

  if (leftIsNum)
    emitTypeAssert(frLeft, hwLeft, TypePred::IsNumber);
  if (rightIsNum)
    emitTypeAssert(frRight, hwRight, TypePred::IsNumber);

  if (slow) {
    // Since HermesValue is NaN-boxed we know that all non-number values will be
    // NaN. So we can conveniently test for non-number values by checking for
    // NaN.
    // x86-64: vucomisd sets PF when either operand is NaN, which is what
    // arm64 reads out of the VS condition code, so this is `jp`.
    static_assert(HERMESVALUE_VERSION == 2, "Non-numbers must be NaN");
    a.vucomisd(hwLeft.xmm(), hwRight.xmm());
    a.jp(slowPathLab);
  }

  // Make sure xmm0, xmm1 are unused.
  syncAndFreeTempReg(HWReg::vecD(0));
  movHWFromFR(HWReg::vecD(0), frLeft);
  syncAndFreeTempReg(HWReg::vecD(1));
  movHWFromFR(HWReg::vecD(1), frRight);

  EMIT_RUNTIME_CALL(*this, double (*)(double, double), _sh_mod_double);
  freeAllFRTempExcept({});
  hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::vecD(0));
  movHWFromHW<false>(hwRes, HWReg::vecD(0));
  frUpdatedWithHW(frRes, hwRes);

  if (!slow)
    return;

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [frRes, frLeft, frRight, hwRes](Emitter &em, SlowPath &sp) {
        em.comment(
            "// mod r%u, r%u, r%u",
            frRes.index(),
            frLeft.index(),
            frRight.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        em.loadFrameAddr(x86::rsi, frLeft);
        em.loadFrameAddr(x86::rdx, frRight);
        EMIT_RUNTIME_CALL(
            em,
            SHLegacyValue (*)(
                SHRuntime *, const SHLegacyValue *, const SHLegacyValue *),
            _sh_ljs_mod_rjs);
        em.movHWFromHW<false>(hwRes, HWReg::gpX(0));
        em.a.jmp(sp.contLab);
      });
}

void Emitter::arithBinOp(
    bool forceNumber,
    FR frRes,
    FR frLeft,
    FR frRight,
    const char *name,
    void (*fast)(
        x86::Assembler &a,
        const x86::Xmm &res,
        const x86::Xmm &dl,
        const x86::Xmm &dr),
    void *slowCall,
    const char *slowCallName) {
  comment(
      "// %s r%u, r%u, r%u",
      name,
      frRes.index(),
      frLeft.index(),
      frRight.index());
  HWReg hwRes, hwLeft, hwRight;
  asmjit::Label slowPathLab;
  asmjit::Label contLab;
  bool leftIsNum, rightIsNum, slow;

  if (forceNumber) {
    frameRegs_[frLeft.index()].localType = FRType::Number;
    frameRegs_[frRight.index()].localType = FRType::Number;
    leftIsNum = rightIsNum = true;
    slow = false;
  } else {
    leftIsNum = isFRKnownNumber(frLeft);
    rightIsNum = isFRKnownNumber(frRight);
    slow = !(rightIsNum && leftIsNum);
  }

  hwLeft = getOrAllocFRInVecD(frLeft, true);
  hwRight = getOrAllocFRInVecD(frRight, true);

  if (leftIsNum)
    emitTypeAssert(frLeft, hwLeft, TypePred::IsNumber);
  if (rightIsNum)
    emitTypeAssert(frRight, hwRight, TypePred::IsNumber);

  if (slow) {
    slowPathLab = newSlowPathLabel();
    contLab = newContLabel();
    syncAllFRTempExcept(frRes != frLeft && frRes != frRight ? frRes : FR());
    syncToFrame(frLeft);
    syncToFrame(frRight);
    freeAllFRTempExcept({});
  }

  hwRes = getOrAllocFRInVecD(frRes, false);
  frUpdatedWithHW(frRes, hwRes, !slow ? FRType::Number : FRType::UnknownPtr);

  if (slow) {
    // Since HermesValue is NaN-boxed we know that all non-number values will be
    // NaN. So we can conveniently test for non-number values by checking for
    // NaN.
    // x86-64: see mod() -- PF is set when either operand is NaN, so `jp`.
    static_assert(HERMESVALUE_VERSION == 2, "Non-numbers must be NaN");
    a.vucomisd(hwLeft.xmm(), hwRight.xmm());
    a.jp(slowPathLab);
  }

  fast(a, hwRes.xmm(), hwLeft.xmm(), hwRight.xmm());

  if (!slow)
    return;

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [name, frRes, frLeft, frRight, hwRes, slowCall, slowCallName](
          Emitter &em, SlowPath &sp) {
        em.comment(
            "// %s r%u, r%u, r%u",
            name,
            frRes.index(),
            frLeft.index(),
            frRight.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        em.loadFrameAddr(x86::rsi, frLeft);
        em.loadFrameAddr(x86::rdx, frRight);
        em.callRuntimeWithSavedIP(slowCall, slowCallName);
        em.movHWFromHW<false>(hwRes, HWReg::gpX(0));
        em.a.jmp(sp.contLab);
      });
}

void Emitter::bitBinOp(
    FR frRes,
    FR frLeft,
    FR frRight,
    bool unsignedRes,
    bool rightInCl,
    const char *name,
    SHLegacyValue (*slowCall)(
        SHRuntime *shr,
        const SHLegacyValue *a,
        const SHLegacyValue *b),
    const char *slowCallName,
    void (*fast)(x86::Assembler &a, const x86::Gp &res, const x86::Gp &right)) {
  comment(
      "// %s r%u, r%u, r%u",
      name,
      frRes.index(),
      frLeft.index(),
      frRight.index());

  // x86-64: a variable shift can only take its count in cl, so for the shifts
  // rcx is vacated here and the right temp is allocated into it. Moving the
  // count into cl later is not an option: by the time the fast body runs, the
  // two temps below are the only live GP temps, so if the left one happened
  // to be rcx the move would destroy the value being shifted.
  //
  // The right temp is allocated first because allocTempGpX() without a hint
  // takes the lowest-numbered free register, which is rcx itself whenever rax
  // is in use. Which of the two temps is which register does not matter
  // otherwise.
  llvh::Optional<HWReg> preferredRight{};
  if (rightInCl) {
    syncAndFreeTempReg(HWReg(x86::rcx));
    preferredRight = HWReg(x86::rcx);
  }
  HWReg hwTempRGpX = allocTempGpX(preferredRight);
  HWReg hwTempLGpX = allocTempGpX();
  // The two steps above are believed to always place the count in rcx, but
  // "believed" is not good enough for a rule the instruction encoding
  // imposes: a shift body reading a stale cl is a silent wrong answer, and
  // an assert would only catch it in a debug build. Check it for real and
  // decline the function instead. The cost is one compare per emitted shift
  // at compile time.
  if (rightInCl && hwTempRGpX != HWReg(x86::rcx))
    unsupported("shift count not in rcx");
  HWReg hwTempLVecD = allocTempVecD();
  HWReg hwTempRVecD = allocTempVecD();

  syncAllFRTempExcept(frRes != frLeft && frRes != frRight ? frRes : FR());
  // TODO: In principle, it should be possible to only sync these in the slow
  // path. If we do that, we have to ensure that the frameUpToDate bit is not
  // set, since subsequent instructions cannot rely on it. To do this, we would
  // need to preserve information for the slow path to know whether they were
  // already sync'd to memory.
  syncToFrame(frLeft);
  syncToFrame(frRight);

  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();

  HWReg hwLeft = getOrAllocFRInVecD(frLeft, true);
  emit_double_is_int(a, hwTempLGpX.gpq(), hwTempLVecD.xmm(), hwLeft.xmm());
  // x86-64: see toInt32 -- an ordered mismatch is in ZF, unordered in PF.
  a.jne(slowPathLab);
  a.jp(slowPathLab);

  // Do the same for the RHS.
  HWReg hwRight = getOrAllocFRInVecD(frRight, true);
  emit_double_is_int(a, hwTempRGpX.gpq(), hwTempRVecD.xmm(), hwRight.xmm());
  a.jne(slowPathLab);
  a.jp(slowPathLab);

  // Done allocating registers. Free them all and allocate the result.
  freeAllFRTempExcept({});
  freeReg(hwTempLGpX);
  freeReg(hwTempRGpX);
  freeReg(hwTempLVecD);
  freeReg(hwTempRVecD);
  HWReg hwRes = getOrAllocFRInVecD(frRes, false);
  frUpdatedWithHW(
      frRes,
      hwRes,
      isFRKnownNumber(frLeft) && isFRKnownNumber(frRight) ? FRType::Number
                                                          : FRType::UnknownPtr);

  // Invoke the fast path, and move the result back as a 32 bit integer.
  // x86-64: the integer ALU is two-operand, and arm64's call site passes the
  // left temp as both the destination and the first source, so the separate
  // destination operand is dropped instead of adding a move that would always
  // be a no-op.
  fast(a, hwTempLGpX.gpq(), hwTempRGpX.gpq());
  emit_int32_to_double(a, hwRes.xmm(), hwTempLGpX.gpq(), unsignedRes);

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [name, frRes, frLeft, frRight, hwRes, slowCall, slowCallName](
          Emitter &em, SlowPath &sp) {
        em.comment(
            "// %s r%u, r%u, r%u",
            name,
            frRes.index(),
            frLeft.index(),
            frRight.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        em.loadFrameAddr(x86::rsi, frLeft);
        em.loadFrameAddr(x86::rdx, frRight);
        em.callRuntimeWithSavedIP((void *)slowCall, slowCallName);
        em.movHWFromHW<false>(hwRes, HWReg::gpX(0));
        em.a.jmp(sp.contLab);
      });
}

void Emitter::setBoolFromCompare(
    const x86::Gp &res,
    x86::CondCode cc,
    bool unorderedPossible) {
  a.set(cc, res.r8());
  if (unorderedPossible &&
      (cc == x86::CondCode::kE || cc == x86::CondCode::kNE)) {
    // Unordered means at least one operand is a NaN, so the two are not
    // equal; ZF says the opposite. PF distinguishes the two cases.
    asmjit::Label doneLab = a.newLabel();
    a.jnp(doneLab);
    a.mov(res.r8(), asmjit::Imm(cc == x86::CondCode::kNE ? 1 : 0));
    a.bind(doneLab);
  }
  // setcc writes 8 bits; the rest of the register must be cleared before the
  // value can be used as a number. Zeroing afterwards rather than before the
  // compare keeps this correct when res aliases one of the compared
  // registers, which happens whenever the result FR is also an operand.
  a.movzx(res.r32(), res.r8());
}

void Emitter::strictEqualImpl(bool invert, FR frRes, FR frLeft, FR frRight) {
  comment(
      "// %s r%u, r%u, r%u",
      !invert ? "StrictEq" : "StrictNEq",
      frRes.index(),
      frLeft.index(),
      frRight.index());

  // Fast path for raw-only comparison. One of the operands is a non-number
  // non-pointer value, meaning we can compare bits directly.
  if (isFRKnownBool(frLeft) || isFRKnownBool(frRight) ||
      isFRKnownOtherNonPtr(frLeft) || isFRKnownOtherNonPtr(frRight)) {
    HWReg hwLeft = getOrAllocFRInGpX(frLeft, true);
    HWReg hwRight = getOrAllocFRInGpX(frRight, true);

    // Evaluate and check the guards before frRes is declared updated below:
    // frRes may alias frLeft/frRight, and frUpdatedWithHW would otherwise
    // make isFRKnownBool/isFRKnownOtherNonPtr true only because of the
    // result declaration, asserting a predicate against a register that
    // still holds the (differently-typed) original operand.
    if (isFRKnownBool(frLeft) || isFRKnownOtherNonPtr(frLeft))
      emitTypeAssert(frLeft, hwLeft, TypePred::BitComparable);
    if (isFRKnownBool(frRight) || isFRKnownOtherNonPtr(frRight))
      emitTypeAssert(frRight, hwRight, TypePred::BitComparable);

    HWReg hwRes = getOrAllocFRInGpX(frRes, false);
    frUpdatedWithHW(frRes, hwRes, FRType::Bool);
    // x86-64: scratch for the bool tag constant (see emit_sh_ljs_bool).
    // Allocated after hwRes and the operands so it cannot alias them.
    HWReg hwTmpTag = allocTempGpX();
    freeReg(hwTmpTag);

    a.cmp(hwLeft.gpq(), hwRight.gpq());
    // An integer compare is never unordered.
    setBoolFromCompare(
        hwRes.gpq(),
        !invert ? x86::CondCode::kE : x86::CondCode::kNE,
        /* unorderedPossible */ false);
    emit_sh_ljs_bool(a, hwRes.gpq(), hwTmpTag.gpq());
    return;
  }

  // Fast path for number (double) comparison.
  // One of the operands is a number, so there's two cases:
  // * It's NaN: it'll compare false against everything which is what we want.
  // * It's not NaN: It won't have NaN tag bits so it'll compare false against
  //   all non-double HVs and correctly compare true against the same number.
  if (isFRKnownNumber(frLeft) || isFRKnownNumber(frRight)) {
    // Do this always, since this could be the end of the BB.
    HWReg hwLeftD = getOrAllocFRInVecD(frLeft, true);
    HWReg hwRightD = getOrAllocFRInVecD(frRight, true);

    // See the raw-bit tier above: evaluate and check the guards before
    // frRes is declared updated, since frRes may alias frLeft/frRight and
    // frUpdatedWithHW would otherwise perturb isFRKnownNumber's answer.
    if (isFRKnownNumber(frLeft))
      emitTypeAssert(frLeft, hwLeftD, TypePred::IsNumber);
    if (isFRKnownNumber(frRight))
      emitTypeAssert(frRight, hwRightD, TypePred::IsNumber);

    HWReg hwRes = getOrAllocFRInGpX(frRes, false);
    frUpdatedWithHW(frRes, hwRes, FRType::Bool);
    HWReg hwTmpTag = allocTempGpX();
    freeReg(hwTmpTag);

    a.vucomisd(hwLeftD.xmm(), hwRightD.xmm());
    // x86-64: nothing has peeled off the unordered case here -- either
    // operand may be a NaN, whether the JS NaN or a NaN-boxed non-number --
    // and kE/kNE alone would answer it backwards.
    setBoolFromCompare(
        hwRes.gpq(),
        !invert ? x86::CondCode::kE : x86::CondCode::kNE,
        /* unorderedPossible */ true);
    emit_sh_ljs_bool(a, hwRes.gpq(), hwTmpTag.gpq());
    return;
  }

  HWReg hwLeftD = getOrAllocFRInVecD(frLeft, true);
  HWReg hwRightD = getOrAllocFRInVecD(frRight, true);

  // Allocate registers used for non-number comparisons.
  HWReg hwLeft = getOrAllocFRInGpX(frLeft, true);
  HWReg hwRight = getOrAllocFRInGpX(frRight, true);
  HWReg hwTmpLeft = allocTempGpX();
  HWReg hwTmpRight = allocTempGpX();
  x86::Gp xTmpLeft = hwTmpLeft.gpq();
  x86::Gp xTmpRight = hwTmpRight.gpq();
  freeReg(hwTmpLeft);
  freeReg(hwTmpRight);

  // Labels for non-number comparisons.
  auto nonNumberLab = a.newLabel();
  auto equalLab = a.newLabel();
  auto notEqualLab = a.newLabel();

  // Set up slow path.
  auto slowPathLab = newSlowPathLabel();
  auto contLab = newContLabel();
  syncAllFRTempExcept(frRes != frLeft && frRes != frRight ? frRes : FR());
  syncToFrame(frLeft);
  syncToFrame(frRight);
  freeAllFRTempExcept({});

  HWReg hwRes = getOrAllocFRInGpX(frRes, false, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes, FRType::Bool);
  x86::Gp xRes = hwRes.gpq();
  // x86-64: scratch for the bool tag constant. It is only ever written on
  // the fall-through (number) path, which is past the `jp` below, so it may
  // safely land on a register the non-number path still reads.
  HWReg hwTmpTag = allocTempGpX();
  freeReg(hwTmpTag);

  // Start by comparing doubles.
  a.vucomisd(hwLeftD.xmm(), hwRightD.xmm());

  // Since HermesValue is NaN-boxed we know that all non-number values will be
  // NaN. So we can conveniently test for non-number values by checking for
  // NaN. x86-64: unordered is reported in PF, which is what arm64 reads out
  // of the VS condition code.
  static_assert(HERMESVALUE_VERSION == 2, "Non-numbers must be NaN");
  a.jp(nonNumberLab);

  // Store the result of the comparison in the lowest bit of xRes.
  // The unordered case has just been routed away, so kE/kNE are exact.
  setBoolFromCompare(
      xRes,
      invert ? x86::CondCode::kNE : x86::CondCode::kE,
      /* unorderedPossible */ false);
  emit_sh_ljs_bool(a, xRes, hwTmpTag.gpq());

  a.jmp(contLab);

  // May be JS NaN (not a number, but really it is a number).
  a.bind(nonNumberLab);
  emit_sh_ljs_is_double(a, hwLeft.gpq(), xTmpLeft);
  // Left is actually the JS NaN, which is never equal to anything.
  // No need to check RHS for JS NaN, because it won't cause false positive
  // on the raw bit check below.
  a.jb(notEqualLab);

  // Compare bits directly.
  // If they match exactly, the two values are equal.
  a.cmp(hwLeft.gpq(), hwRight.gpq());
  a.je(equalLab);

  // First compare the tags. If they don't match, the two values are NOT equal.
  emit_sh_ljs_get_tag(a, xTmpLeft, hwLeft.gpq());
  emit_sh_ljs_get_tag(a, xTmpRight, hwRight.gpq());
  a.cmp(xTmpLeft, xTmpRight);
  a.jne(notEqualLab);

  // If the LHS is either a non-pointer or an object, we can compare raw values
  // only. We've already checked and we know that the raw values are not the
  // same, so if this is a non-pointer or an object, then the two values are NOT
  // strictly equal.
  emit_sh_ljs_tag_is_pointer(a, xTmpLeft);
  a.jb(notEqualLab);
  emit_sh_ljs_tag_is_object(a, xTmpLeft);
  a.je(notEqualLab);

  // Now we know that the LHS is a non-object pointer.

  // Fast string path: string inequality can be easily determined by checking
  // the lengths. If the LHS isn't a string, go to slow path.
  emit_sh_ljs_tag_is_string(a, xTmpLeft);
  a.jne(slowPathLab);

  emit_stringprim_get_length_and_flags(a, xTmpLeft, hwLeft.gpq());
  emit_stringprim_get_length_and_flags(a, xTmpRight, hwRight.gpq());
  // XOR the lengths together and mask the result.
  // xTmpLeft will be nonzero if the lengths don't match.
  // x86-64: the loads zero-extend, so 32-bit operations are equivalent to
  // arm64's 64-bit ones here, and the mask fits in an imm32.
  a.xor_(xTmpLeft.r32(), xTmpRight.r32());
  a.and_(
      xTmpLeft.r32(), asmjit::Imm(RuntimeOffsets::stringPrimitiveLengthMask));
  // Length mismatch means we're done, the two values are NOT equal.
  a.jnz(notEqualLab);
  a.jmp(slowPathLab);

  // Jump here if the result should be "equal".
  // Returns true if not inverted, false if inverted.
  a.bind(equalLab);
  emit_sh_ljs_bool_const(a, xRes, !invert);
  a.jmp(contLab);

  // Jump here if the result should be "not equal".
  // Returns false if not inverted, true if inverted.
  a.bind(notEqualLab);
  emit_sh_ljs_bool_const(a, xRes, invert);

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [invert, frRes, frLeft, frRight, hwRes](Emitter &em, SlowPath &sp) {
        em.comment(
            "// Slow path: %s r%u, r%u, r%u",
            !invert ? "StrictEq" : "StrictNEq",
            frRes.index(),
            frLeft.index(),
            frRight.index());
        em.a.bind(sp.slowPathLab);
        // _sh_ljs_strict_equal takes its arguments by value.
        em._loadFrame(HWReg(x86::rdi), frLeft);
        em._loadFrame(HWReg(x86::rsi), frRight);
        em.callRuntimeWithSavedIP(
            (void *)_sh_ljs_strict_equal, "_sh_ljs_strict_equal");

        // x86-64: SysV leaves everything above al undefined for a bool
        // return, so zero-extend before using it as a 0/1 value.
        em.a.movzx(hwRes.gpq().r32(), x86::al);
        // Invert the slow path result if needed.
        if (invert)
          em.a.xor_(hwRes.gpq().r32(), asmjit::Imm(1));

        // Comparison functions return bool, so encode it. x86-64: the tag
        // constant needs a scratch register; xScratch is dead here, since
        // its only role is holding the call target.
        emit_sh_ljs_bool(em.a, hwRes.gpq(), xScratch);
        em.a.jmp(sp.contLab);
      });
}

void Emitter::compareImpl(
    FR frRes,
    FR frLeft,
    FR frRight,
    const char *name,
    x86::CondCode condCode,
    bool swapOperands,
    void *slowCall,
    const char *slowCallName,
    bool invSlow,
    bool passArgsByVal) {
  comment(
      "// %s r%u, r%u, r%u",
      name,
      frRes.index(),
      frLeft.index(),
      frRight.index());
  HWReg hwLeft, hwRight;
  asmjit::Label slowPathLab;
  asmjit::Label contLab;
  bool leftIsNum, rightIsNum, slow;

  leftIsNum = isFRKnownNumber(frLeft);
  rightIsNum = isFRKnownNumber(frRight);
  slow = !(rightIsNum && leftIsNum);

  hwLeft = getOrAllocFRInVecD(frLeft, true);
  hwRight = getOrAllocFRInVecD(frRight, true);
  if (leftIsNum)
    emitTypeAssert(frLeft, hwLeft, TypePred::IsNumber);
  if (rightIsNum)
    emitTypeAssert(frRight, hwRight, TypePred::IsNumber);
  if (slow) {
    slowPathLab = newSlowPathLabel();
    contLab = newContLabel();
    syncAllFRTempExcept(frRes != frLeft && frRes != frRight ? frRes : FR());
    syncToFrame(frLeft);
    syncToFrame(frRight);
    freeAllFRTempExcept({});
  }

  HWReg hwRes = getOrAllocFRInGpX(frRes, false, HWReg::gpX(0));
  x86::Gp xRes = hwRes.gpq();
  // x86-64: scratch for the bool tag constant (see emit_sh_ljs_bool).
  // Allocated before the compare so that a spill cannot disturb the flags.
  HWReg hwTmpTag = allocTempGpX();
  freeReg(hwTmpTag);

  // x86-64: "less" and "less or equal" compare the reversed operands, so
  // that the condition stays in the above family, which is the family that
  // is false on unordered. See the condition mapping in the design.
  a.vucomisd(
      swapOperands ? hwRight.xmm() : hwLeft.xmm(),
      swapOperands ? hwLeft.xmm() : hwRight.xmm());

  if (slow) {
    // Since HermesValue is NaN-boxed we know that all non-number values will be
    // NaN. So we can conveniently test for non-number values by checking for
    // NaN. x86-64: unordered is reported in PF.
    static_assert(HERMESVALUE_VERSION == 2, "Non-numbers must be NaN");
    a.jp(slowPathLab);
  }

  // Store the result of the comparison in the lowest bit of xRes. When there
  // is no slow path both operands are statically numbers, but either can
  // still be the JS NaN, so the unordered case has not been ruled out.
  setBoolFromCompare(xRes, condCode, /* unorderedPossible */ !slow);

  // Encode bool.
  emit_sh_ljs_bool(a, xRes, hwTmpTag.gpq());
  frUpdatedWithHW(frRes, hwRes, FRType::Bool);

  if (!slow)
    return;

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [name,
       frRes,
       frLeft,
       frRight,
       hwRes,
       invSlow,
       passArgsByVal,
       slowCall,
       slowCallName](Emitter &em, SlowPath &sp) {
        em.comment(
            "// Slow path: j_%s r%u, r%u, r%u",
            name,
            frRes.index(),
            frLeft.index(),
            frRight.index());
        em.a.bind(sp.slowPathLab);
        if (passArgsByVal) {
          em._loadFrame(HWReg(x86::rdi), frLeft);
          em._loadFrame(HWReg(x86::rsi), frRight);
        } else {
          em.a.mov(x86::rdi, xRuntime);
          em.loadFrameAddr(x86::rsi, frLeft);
          em.loadFrameAddr(x86::rdx, frRight);
        }
        em.callRuntimeWithSavedIP(slowCall, slowCallName);

        // x86-64: SysV leaves everything above al undefined for a bool
        // return, so zero-extend before using it as a 0/1 value.
        em.a.movzx(hwRes.gpq().r32(), x86::al);
        // Invert the slow path result if needed.
        if (invSlow)
          em.a.xor_(hwRes.gpq().r32(), asmjit::Imm(1));

        // Comparison functions return bool, so encode it. x86-64: xScratch
        // is dead after the call and holds the tag constant.
        emit_sh_ljs_bool(em.a, hwRes.gpq(), xScratch);
        em.a.jmp(sp.contLab);
      });
}

} // namespace hermes::vm::x86_64
#endif // HERMESVM_JIT_X86_64

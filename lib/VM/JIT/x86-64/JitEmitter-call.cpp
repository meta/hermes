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

#include "hermes/FrontEndDefs/Builtins.h"
#include "hermes/VM/VTable.h"

namespace hermes::vm::x86_64 {

void Emitter::createThis(
    FR frRes,
    FR frCallee,
    FR frNewTarget,
    uint8_t cacheIdx,
    uint32_t shapeTableIndex) {
  comment(
      "// CreateThis r%u, r%u, r%u, cache %u, shape %u",
      frRes.index(),
      frCallee.index(),
      frNewTarget.index(),
      cacheIdx,
      shapeTableIndex);

  syncAllFRTempExcept(frRes != frCallee && frRes != frNewTarget ? frRes : FR());
  syncToFrame(frCallee);
  syncToFrame(frNewTarget);
  freeAllFRTempExcept({});

  a.mov(x86::rdi, xRuntime);
  loadFrameAddr(x86::rsi, frCallee);
  loadFrameAddr(x86::rdx, frNewTarget);
  if (cacheIdx == hbc::PROPERTY_CACHING_DISABLED) {
    a.xor_(x86::ecx, x86::ecx);
  } else {
    // x86-64: the RO data entry is addressed RIP-relative, so unlike arm64
    // this is a plain load with no base register to set up, and the cache
    // offset folds into a single add with an imm32.
    a.mov(x86::rcx, x86::qword_ptr(roDataLabel_, roOfsReadPropertyCachePtr_));
    if (cacheIdx != 0)
      a.add(x86::rcx, asmjit::Imm(sizeof(SHReadPropertyCacheEntry) * cacheIdx));
  }
  if (shapeTableIndex == hbc::SHAPE_TABLE_CACHING_DISABLED) {
    a.xor_(x86::r8d, x86::r8d);
  } else {
    // Pass the address of the RuntimeModule's class cache slot.
    static_assert(
        std::is_same_v<
            TransparentConservativeVector<WeakRoot<HiddenClass>>,
            RuntimeOffsets::RuntimeModuleObjectLiteralHiddenClassesType>,
        "objectLiteralHiddenClasses_ must be transparent");
    loadBits64InGp(
        x86::r8, (uint64_t)codeBlock_->getRuntimeModule(), "RuntimeModule");
    a.mov(
        x86::r8,
        x86::qword_ptr(
            x86::r8, RuntimeOffsets::runtimeModuleObjectLiteralHiddenClasses));
    // x86-64: the offset folds into a single add with an imm32, where arm64
    // needs emit_add_imm_u24().
    size_t shapeOfs = (size_t)shapeTableIndex * sizeof(WeakRoot<HiddenClass>);
    assert(
        shapeOfs <= (size_t)INT32_MAX &&
        "shape table offset must fit an imm32");
    if (shapeOfs != 0)
      a.add(x86::r8, asmjit::Imm((int32_t)shapeOfs));
  }
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(
          SHRuntime *,
          SHLegacyValue *,
          SHLegacyValue *,
          SHReadPropertyCacheEntry *,
          SHCompressedPointer *),
      _sh_ljs_create_this);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::selectObject(FR frRes, FR frThis, FR frConstructed) {
  comment(
      "// SelectObject r%u, r%u, r%u",
      frRes.index(),
      frThis.index(),
      frConstructed.index());

  HWReg hwConstructed = getOrAllocFRInGpX(frConstructed, true);
  HWReg hwThis = getOrAllocFRInGpX(frThis, true);

  // Check if frConstructed is an object.
  // Get the tag bits in the temp by right shifting.
  HWReg hwTmpConstructedTag = allocTempGpX();
  emit_sh_ljs_is_object(a, hwTmpConstructedTag.gpq(), hwConstructed.gpq());
  freeReg(hwTmpConstructedTag);

  // The flags set above are live across this allocation, which is exactly
  // the shape emitTypeAssert's doc comment names as the counterexample to
  // "flags are dead at emitter boundaries". It is safe only because every
  // instruction getOrAllocFRInGpX can emit is a register-to-register or
  // memory move, none of which writes EFLAGS. This ordering is arm64's,
  // kept deliberately: allocating the result first would let it take the
  // register the tag temp is freed from and reorder nothing useful.
  HWReg hwRes = getOrAllocFRInGpX(frRes, false);
  // If it is an object, use Constructed, otherwise use This.
  // x86-64: arm64's three-operand csel becomes a mov plus a cmov, which is
  // two-operand and therefore has to be arranged around the aliasing of the
  // result with either source. When the result already is Constructed, the
  // move would destroy the value the cmov must keep, so the sense of the
  // condition is inverted and This is the conditionally moved operand
  // instead.
  if (hwRes == hwConstructed) {
    a.cmovne(hwRes.gpq(), hwThis.gpq());
  } else {
    movHWFromHW<false>(hwRes, hwThis);
    a.cmove(hwRes.gpq(), hwConstructed.gpq());
  }

  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::loadThisNS(FR frRes) {
  comment("// LoadThisNS r%u", frRes.index());
  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();
  syncAllFRTempExcept(frRes);
  freeAllFRTempExcept({});

  HWReg hwRes = getOrAllocFRInGpX(frRes, false);
  frUpdatedWithHW(frRes, hwRes, FRType::Pointer);
  HWReg hwTemp = allocTempGpX();
  freeReg(hwTemp);

  // Load the ThisArg from the stack.
  // x86-64: the negative frame offset is just a displacement, so arm64's
  // ldur is a plain mov.
  a.mov(
      hwRes.gpq(),
      x86::qword_ptr(
          xFrame, StackFrameLayout::ThisArg * (int)sizeof(SHLegacyValue)));
  // If it is an object, we are done.
  emit_sh_ljs_is_object(a, hwTemp.gpq(), hwRes.gpq());
  a.jne(slowPathLab);

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [frRes, hwRes](Emitter &em, SlowPath &sp) {
        em.comment("// Slow path: LoadThisNS r%u", frRes.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        em.a.mov(
            x86::rsi,
            x86::qword_ptr(
                xFrame,
                StackFrameLayout::ThisArg * (int)sizeof(SHLegacyValue)));
        EMIT_RUNTIME_CALL(
            em,
            SHLegacyValue (*)(SHRuntime *, SHLegacyValue),
            _sh_ljs_coerce_this_ns);
        em.movHWFromHW<false>(hwRes, HWReg::gpX(0));
        em.a.jmp(sp.contLab);
      });
}

void Emitter::coerceThisNS(FR frRes, FR frThis) {
  comment("// CoerceThisNS r%u, r%u", frRes.index(), frThis.index());
  asmjit::Label slowPathLab = newSlowPathLabel();
  asmjit::Label contLab = newContLabel();
  HWReg hwThis = getOrAllocFRInGpX(frThis, true);

  // Sync all registers. Note that we don't need to check for frRes == frThis
  // here, because frThis is sync'd unconditionally below.
  syncAllFRTempExcept(frRes);
  syncToFrame(frThis);

  // Allocate a temporary register. This must not be the same as hwThis, but may
  // be the same as hwRes.
  HWReg hwTemp = allocTempGpX();
  freeReg(hwTemp);

  // We don't free frRes so that if it is the same as frThis, the register is
  // simply persisted and we do not need to perform a move in the fast path.
  freeAllFRTempExcept(frRes);

  HWReg hwRes = getOrAllocFRInGpX(frRes, false);
  frUpdatedWithHW(frRes, hwRes, FRType::Pointer);
  // If the operand is an object, we are done, otherwise, go to the slow path.
  emit_sh_ljs_is_object(a, hwTemp.gpq(), hwThis.gpq());
  a.jne(slowPathLab);

  movHWFromHW<false>(hwRes, hwThis);

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [frRes, frThis, hwRes](Emitter &em, SlowPath &sp) {
        em.comment(
            "// Slow path: CoerceThis r%u, r%u", frRes.index(), frThis.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        em._loadFrame(HWReg(x86::rsi), frThis);
        EMIT_RUNTIME_CALL(
            em,
            SHLegacyValue (*)(SHRuntime *, SHLegacyValue),
            _sh_ljs_coerce_this_ns);
        em.movHWFromHW<false>(hwRes, HWReg::gpX(0));
        em.a.jmp(sp.contLab);
      });
}

void Emitter::getNewTarget(FR frRes) {
  comment("// GetNewTarget r%u", frRes.index());
  HWReg hwRes = getOrAllocFRInGpX(frRes, false);
  a.mov(
      hwRes.gpq(),
      x86::qword_ptr(
          xFrame,
          (int)StackFrameLayout::NewTarget * (int)sizeof(SHLegacyValue)));
  frUpdatedWithHW(frRes, hwRes);
}

// See the register contract in the header.
void Emitter::callImpl(FR frRes, FR frCallee) {
  uint32_t nRegs = frameRegs_.size();

  emitIncrementCounter(JitCounter::NumCall);
  FR calleeFrameArg{nRegs + hbc::StackFrameLayout::CalleeClosureOrCB};

  // Store the callee to the right location in the frame, if it isn't already
  // there.
  if (frCallee != calleeFrameArg) {
    // Free any temp register before we mov into it so movFRFromHW stores
    // directly to the frame.
    freeFRTemp(calleeFrameArg);
    auto calleeReg = getOrAllocFRInAnyReg(frCallee, true);
    movFRFromHW(
        calleeFrameArg, calleeReg, frameRegs_[frCallee.index()].localType);
  }

  static_assert(
      HERMESVALUE_VERSION == 2,
      "Native pointers must be encoded without modification");

  FR previousFrameArg{nRegs + hbc::StackFrameLayout::PreviousFrame};
  // Free any existing temp so we store directly.
  freeFRTemp(previousFrameArg);
  movFRFromHW(previousFrameArg, HWReg(xFrame), FRType::OtherNonPtr);

  FR savedIPArg{nRegs + hbc::StackFrameLayout::SavedIP};
  // Since we need a register to compute the IP in anyway, it is convenient to
  // just use any existing one for the SavedIP slot, and let the syncAllFRTemp
  // below write it to memory.
  auto savedIPReg = getOrAllocFRInGpX(savedIPArg, false);

  // Save the current IP in both the SavedIP slot and the runtime.
  getBytecodeIP(savedIPReg.gpq());
  frUpdatedWithHW(savedIPArg, savedIPReg, FRType::OtherNonPtr);
  a.mov(x86::qword_ptr(xRuntime, RuntimeOffsets::currentIP), savedIPReg.gpq());

  FR savedCodeBlockArg = FR{nRegs + hbc::StackFrameLayout::SavedCodeBlock};
  // TODO: We should be able to store the zero directly.
  auto savedCodeBlockReg = getOrAllocFRInGpX(savedCodeBlockArg, false);
  frUpdatedWithHW(savedCodeBlockArg, savedCodeBlockReg, FRType::OtherNonPtr);
  // No flags are live here, so the shorter zeroing idiom is fine.
  a.xor_(savedCodeBlockReg.gpq().r32(), savedCodeBlockReg.gpq().r32());

  FR shLocalsArg{nRegs + hbc::StackFrameLayout::SHLocals};
  // Free any existing temp so we store directly.
  freeFRTemp(shLocalsArg);
  movFRFromHW(shLocalsArg, savedCodeBlockReg, FRType::OtherNonPtr);

#ifndef NDEBUG
  // No need to sync the set up call stack to the frame memory,
  // because it these registers can't have global registers.
  for (uint32_t i = 0; i < StackFrameLayout::CallerExtraRegistersAtEnd; ++i) {
    assert(
        !frameRegs_[nRegs - i - 1].globalReg &&
        "frame regs are not number/non-pointer so can't have global reg");
  }
#endif

  auto hwCallee = getOrAllocFRInGpX(frCallee, true);
  auto hwTemp = allocTempGpX();
  auto xTemp = hwTemp.gpq();
  syncAllFRTempExcept(FR());
  freeAllFRTempExcept({});
  freeReg(hwTemp);
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);

  // If we have not already created a slow path for non-object calls, do so now.
  if (!nonObjCallLabel_.isValid()) {
    nonObjCallLabel_ = newSlowPathLabel();
    slowPaths_.emplace_back(
        nonObjCallLabel_,
        /* emittingIP */ nullptr,
        [](Emitter &em, SlowPath &sp) {
          em.comment("// Throw on non-object call");
          em.a.bind(sp.slowPathLab);
          em.a.mov(x86::rdi, xRuntime);
          // The IP is already saved by the call setup, no need to save it
          // again.
          EMIT_RUNTIME_CALL_WITHOUT_SAVED_IP(
              em, void (*)(SHRuntime *), _jit_throw_non_object_call);
          // The call does not return.
        });
  }

  auto slowPathLab = newSlowPathLabel();
  auto contLab = newContLabel();

  // Check if the callee is a JSFunction we have already JITted.
  emit_sh_ljs_is_object(a, xTemp, hwCallee.gpq());
  a.jne(nonObjCallLabel_);

  // We can now use any temp registers we want, because everything has been
  // sync'd and we are done with hwCallee. We keep the callee in rsi so that we
  // don't have to move it in the slow path.
  //
  // Both helpers below clobber EFLAGS, where their arm64 counterparts leave
  // NZCV alone, so the non-object branch above must be taken before either
  // of them runs. That is arm64's order too, for the independent reason
  // that neither may run on a value that is not a pointer at all.
  emit_sh_ljs_get_pointer(a, x86::rsi, hwCallee.gpq());
  emit_gccell_get_kind(a, x86::rax, x86::rsi);

  // Check if it is a JSFunction. rdx is free until the call target lands in
  // it below, so it doubles as the range check's temp.
  emit_cellkind_in_range(
      a,
      /* temp */ x86::rdx,
      /* input */ x86::rax,
      CellKind::CodeBlockFunctionKind_first,
      CellKind::CodeBlockFunctionKind_last);
  a.ja(slowPathLab);

  // Check if the JSFunction has already been JIT compiled.
  a.mov(
      x86::rdx, x86::qword_ptr(x86::rsi, RuntimeOffsets::jsFunctionCodeBlock));
  a.mov(x86::rdx, x86::qword_ptr(x86::rdx, RuntimeOffsets::codeBlockJitPtr));
  a.test(x86::rdx, x86::rdx);
  a.jz(slowPathLab);

  // We have a JIT compiled function, call it.
  a.bind(contLab);
  // Both the fast and slow paths have prepared any arguments except the first
  // one, which is Runtime, and placed the call target in rdx. Call it.
  a.mov(x86::rdi, xRuntime);
  a.call(x86::rdx);
  movHWFromHW<false>(hwRes, HWReg::gpX(0));

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      /* emittingIP */ nullptr,
      [frRes, frCallee](Emitter &em, SlowPath &sp) {
        em.comment(
            "// Slow path: CallImpl r%u, r%u", frRes.index(), frCallee.index());
        em.a.bind(sp.slowPathLab);
        em.emitIncrementCounter(JitCounter::NumCallSlow);
        em.emitIncrementSlowCallKindCounter();
        // x86-64: a scaled-index memory operand has no absolute-address
        // form, so the table's base is materialized into xScratch first.
        // eax still contains the callee CellKind from the fast path.
        em.loadBits64InGp(
            xScratch, (uint64_t)&VTable::jitCallArray, "VTable::jitCallArray");
        // Load the jitCall function.
        em.a.mov(x86::rdx, x86::qword_ptr(xScratch, x86::rax, 3));
        // rsi already contains the Callable* from the fast path.
        em.a.jmp(sp.contLab);
      });
}

void Emitter::call(FR frRes, FR frCallee, uint32_t argc) {
  comment("// Call r%u, r%u, %u", frRes.index(), frCallee.index(), argc);
  uint32_t nRegs = frameRegs_.size();

  // Store undefined as the new target.
  FR ntFrameArg{nRegs + hbc::StackFrameLayout::NewTarget};
  loadConstBits64(
      ntFrameArg, _sh_ljs_undefined().raw, FRType::UnknownNonPtr, "undefined");

  FR argcFrameArg{nRegs + hbc::StackFrameLayout::ArgCount};
  static_assert(HERMESVALUE_VERSION == 2, "Native u32 must not need encoding");
  // The bytecode arg count includes "this", but the frame one does not, so
  // subtract 1.
  loadConstBits64(argcFrameArg, argc - 1, FRType::OtherNonPtr, "argCount");

#ifndef NDEBUG
  // No need to sync the set up call stack to the frame memory,
  // because it these registers can't have global registers.
  for (uint32_t i = 0; i < argc; ++i) {
    assert(
        !frameRegs_[nRegs + hbc::StackFrameLayout::ThisArg - i].globalReg &&
        "frame regs are not number/non-pointer so can't have global reg");
  }
#endif

  callImpl(frRes, frCallee);
}

void Emitter::callN(FR frRes, FR frCallee, llvh::ArrayRef<FR> args) {
  comment(
      "// Call%zu r%u, r%u, ...args",
      args.size(),
      frRes.index(),
      frCallee.index());
  uint32_t nRegs = frameRegs_.size();

  for (uint32_t i = 0; i < args.size(); ++i) {
    auto argLoc = FR{nRegs + hbc::StackFrameLayout::ThisArg - i};

    if (args[i] != argLoc) {
      // Free any temp register before we mov into it so movFRFromHW stores
      // directly to the frame.
      freeFRTemp(argLoc);
      auto argReg = getOrAllocFRInAnyReg(args[i], true);
      movFRFromHW(argLoc, argReg, frameRegs_[args[i].index()].localType);
    }
    assert(
        !frameRegs_[argLoc.index()].globalReg &&
        "frame regs are not number/non-pointer so can't have global reg");
  }

  // Get a register for the new target.
  FR ntFrameArg{nRegs + hbc::StackFrameLayout::NewTarget};
  loadConstBits64(
      ntFrameArg, _sh_ljs_undefined().raw, FRType::UnknownNonPtr, "undefined");
  syncToFrame(ntFrameArg);

  FR argcFrameArg{nRegs + hbc::StackFrameLayout::ArgCount};
  static_assert(HERMESVALUE_VERSION == 2, "Native u32 must not need encoding");
  // The bytecode arg count includes "this", but the frame one does not, so
  // subtract 1.
  loadConstBits64(
      argcFrameArg, args.size() - 1, FRType::OtherNonPtr, "argCount");

  callImpl(frRes, frCallee);
}

void Emitter::callBuiltin(FR frRes, uint32_t builtinIndex, uint32_t argc) {
  comment(
      "// CallBuiltin r%u, %s, %u",
      frRes.index(),
      getBuiltinMethodName(builtinIndex),
      argc);
  // The ThisArg slot is not populated by bytecode; _jit_call_builtin
  // initializes it. Note that the syncAllFRTempExcept({}) below does write
  // the ThisArg FR to memory along with everything else — it is simply that
  // whatever it writes there is dead, since the handler overwrites it.
#ifndef NDEBUG
  uint32_t nRegs = frameRegs_.size();

  // No need to sync the set up call stack to the frame memory,
  // because it these registers can't have global registers.
  for (uint32_t i = 0; i < argc; ++i) {
    assert(
        !frameRegs_[nRegs + hbc::StackFrameLayout::ThisArg - i].globalReg &&
        "frame regs are not number/non-pointer so can't have global reg");
  }
#endif

  syncAllFRTempExcept({});
  freeAllFRTempExcept({});

  a.mov(x86::rdi, xRuntime);
  a.mov(x86::rsi, xFrame);
  // The bytecode arg count includes "this", but the SH one does not, so
  // subtract 1.
  a.mov(x86::edx, asmjit::Imm(argc - 1));
  a.mov(x86::ecx, asmjit::Imm(builtinIndex));
  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(SHRuntime *, SHLegacyValue *, uint32_t, uint32_t),
      _jit_call_builtin);
  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false, HWReg::gpX(0));
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::callWithNewTarget(
    FR frRes,
    FR frCallee,
    FR frNewTarget,
    uint32_t argc) {
  comment(
      "// CallWithNewTarget r%u, r%u, r%u, %u",
      frRes.index(),
      frCallee.index(),
      frNewTarget.index(),
      argc);
  uint32_t nRegs = frameRegs_.size();

  FR ntFrameArg{nRegs + hbc::StackFrameLayout::NewTarget};
  // Store the new target to the right location in the frame.
  if (ntFrameArg != frNewTarget) {
    // Free the register before we mov into it so we store directly to the
    // frame.
    freeFRTemp(ntFrameArg);
    auto newTargetReg = getOrAllocFRInAnyReg(frNewTarget, true);
    movFRFromHW(
        ntFrameArg, newTargetReg, frameRegs_[frNewTarget.index()].localType);
  }

  FR argcFrameArg{nRegs + hbc::StackFrameLayout::ArgCount};
  static_assert(HERMESVALUE_VERSION == 2, "Native u32 must not need encoding");
  // The bytecode arg count includes "this", but the frame one does not, so
  // subtract 1.
  loadConstBits64(argcFrameArg, argc - 1, FRType::OtherNonPtr, "argCount");

#ifndef NDEBUG
  // No need to sync the set up call stack to the frame memory,
  // because it these registers can't have global registers.
  for (uint32_t i = 0; i < argc; ++i) {
    assert(
        !frameRegs_[nRegs + hbc::StackFrameLayout::ThisArg - i].globalReg &&
        "frame regs are not number/non-pointer so can't have global reg");
  }
#endif

  callImpl(frRes, frCallee);
}

void Emitter::callWithNewTargetLong(
    FR frRes,
    FR frCallee,
    FR frNewTarget,
    FR frArgc) {
  comment(
      "// CallWithNewTarget r%u, r%u, r%u, r%u",
      frRes.index(),
      frCallee.index(),
      frNewTarget.index(),
      frArgc.index());
  uint32_t nRegs = frameRegs_.size();

  FR ntFrameArg{nRegs + hbc::StackFrameLayout::NewTarget};
  // Store the new target to the right location in the frame.
  if (ntFrameArg != frNewTarget) {
    // Free the register before we mov into it so we store directly to the
    // frame.
    freeFRTemp(ntFrameArg);
    auto newTargetReg = getOrAllocFRInAnyReg(frNewTarget, true);
    movFRFromHW(
        ntFrameArg, newTargetReg, frameRegs_[frNewTarget.index()].localType);
  }

  HWReg hwArgc = getOrAllocFRInVecD(frArgc, true);
  FR argcFrameArg{nRegs + hbc::StackFrameLayout::ArgCount};
  HWReg hwArgcArg = getOrAllocFRInGpX(argcFrameArg, false);
  frUpdatedWithHW(argcFrameArg, hwArgcArg, FRType::OtherNonPtr);

  emitTypeAssert(frArgc, hwArgc, TypePred::IsNumber);
  static_assert(HERMESVALUE_VERSION == 2, "Native u32 must not need encoding");
  // x86-64: there is no unsigned float-to-integer conversion, so arm64's
  // fcvtzu becomes the signed 64-bit vcvttsd2si. The two differ only for
  // arg counts at or above 2^63, which no frame can describe.
  a.vcvttsd2si(hwArgcArg.gpq(), hwArgc.xmm());
  // The bytecode arg count includes "this", but the frame one does not, so
  // subtract 1.
  // x86-64: `sub` also writes EFLAGS, where arm64's three-operand `sub`
  // does not touch flags at all.
  a.sub(hwArgcArg.gpq(), asmjit::Imm(1));

  callImpl(frRes, frCallee);
}

void Emitter::callRequire(FR frRes, FR frRequireFunc, uint32_t modIndex) {
  comment(
      "// CallRequire r%u, r%u, %u",
      frRes.index(),
      frRequireFunc.index(),
      modIndex);

  syncAllFRTempExcept(frRes != frRequireFunc ? frRes : FR());
  syncToFrame(frRequireFunc);
  freeAllFRTempExcept({});

  a.mov(x86::rdi, xRuntime);
  loadBits64InGp(
      x86::rsi,
      (uint64_t)codeBlock_->getRuntimeModule() +
          RuntimeOffsets::runtimeModuleModuleCache,
      "cacheData");
  loadFrameAddr(x86::rdx, frRequireFunc);
  a.mov(x86::ecx, asmjit::Imm(modIndex));

  EMIT_RUNTIME_CALL(
      *this,
      SHLegacyValue (*)(
          SHRuntime *, SHArrayStorage **, SHLegacyValue *, uint32_t),
      _sh_ljs_callRequire);

  HWReg hwRes = getOrAllocFRInAnyReg(frRes, false);
  movHWFromHW<false>(hwRes, HWReg::gpX(0));
  frUpdatedWithHW(frRes, hwRes);
}

void Emitter::getBuiltinClosure(FR frRes, uint32_t builtinIndex) {
  comment(
      "// GetBuiltinClosure r%u, %s",
      frRes.index(),
      getBuiltinMethodName(builtinIndex));
  auto hwRes = getOrAllocFRInGpX(frRes, false);
  frUpdatedWithHW(frRes, hwRes, FRType::Pointer);
  // Load the closure pointer and add the object tag.
  emit_load_builtin_closure(a, hwRes.gpq(), builtinIndex);
  emit_sh_ljs_object(a, hwRes.gpq());
}

} // namespace hermes::vm::x86_64
#endif // HERMESVM_JIT_X86_64

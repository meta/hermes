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

namespace hermes::vm::x86_64 {

void Emitter::catchInst(FR frRes) {
  comment("// Catch r%u", frRes.index());

  HWReg hwTemp = allocTempGpX();
  HWReg hwRes = getOrAllocFRInGpX(frRes, false);
  frUpdatedWithHW(frRes, hwRes);
  freeReg(hwTemp);

  // Catch simply returns the thrown value and clears it.
  //
  // Nothing here touches the SHJmpBuf or the saved SHLocals: this runs at the
  // top of a handler basic block, which emitCatchTable() has already jumped
  // to AFTER calling _jit_find_catch_target(), and that call is what hands
  // the saved SHLocals to _sh_catch_no_pop(). See emitCatchTable() for why
  // its own rsp-relative accesses are sound.

  // Read thrown value.
  a.mov(hwRes.gpq(), x86::qword_ptr(xRuntime, RuntimeOffsets::thrownValue));
  // Clear thrown value.
  loadBits64InGp(hwTemp.gpq(), _sh_ljs_empty().raw, "empty");
  a.mov(x86::qword_ptr(xRuntime, RuntimeOffsets::thrownValue), hwTemp.gpq());
}

void Emitter::ret(FR frValue) {
  // kGPReturnStash (rbx) is the return value stash (the x21 analogue):
  // leave() moves it to rax, the SysV return register, after restoring the
  // frame. Clobbering rbx here without invalidating any FR's HWReg state is
  // sound only because every path through this mov terminates at leave():
  // there is no fall-through or branch back into the FR allocator that
  // could observe rbx's old, now-stale contents.
  movHWFromFR(HWReg::gpX(kGPReturnStash), frValue);
  a.jmp(returnLabel_);
}

void Emitter::throwInst(FR frInput) {
  comment("// Throw r%u", frInput.index());

  // We have to sync registers when the throw is inside a try region
  // because we could read from the FRs again in this function.
  if (isInTry())
    syncAllFRTempExcept({});
  // x86-64: the by-value SHLegacyValue argument goes in rsi, and xRuntime
  // (r15) is neither an argument register nor a temp, so loading rdi after
  // frInput cannot disturb it.
  movHWFromFR(HWReg(x86::rsi), frInput);
  freeAllFRTempExcept({});

  a.mov(x86::rdi, xRuntime);
  EMIT_RUNTIME_CALL(*this, void (*)(SHRuntime *, SHLegacyValue), _sh_throw);
}

void Emitter::throwIfEmptyUndefinedImpl(FR frRes, FR frInput, bool empty) {
  comment(
      "// %s r%u, r%u",
      empty ? "ThrowIfEmpty" : "ThrowIfUndefined",
      frRes.index(),
      frInput.index());

  asmjit::Label slowPathLab = newSlowPathLabel();

  // We have to sync registers when the throw is inside a try region
  // because we could read from the FRs again in this function.
  if (isInTry())
    syncAllFRTempExcept(frRes != frInput ? frRes : FR());
  HWReg hwInput = getOrAllocFRInGpX(frInput, true);
  HWReg hwTemp = allocTempGpX();
  if (isInTry())
    freeAllFRTempExcept({});
  freeReg(hwTemp);

  if (empty)
    emit_sh_ljs_is_empty(a, hwTemp.gpq(), hwInput.gpq());
  else
    emit_sh_ljs_is_undefined(a, hwTemp.gpq(), hwInput.gpq());
  a.je(slowPathLab);

  HWReg hwRes = getOrAllocFRInGpX(frRes, false);
  movHWFromHW<false>(hwRes, hwInput);
  frUpdatedWithHW(frRes, hwRes);

  slowPaths_.emplace_back(
      slowPathLab,
      emittingIP,
      [empty, frRes, frInput](Emitter &em, SlowPath &sp) {
        em.comment(
            "// Slow path: %s r%u, r%u",
            empty ? "ThrowIfEmpty" : "ThrowIfUndefined",
            frRes.index(),
            frInput.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        EMIT_RUNTIME_CALL(em, void (*)(SHRuntime *), _sh_throw_empty);
        // Call does not return.
      });
}

void Emitter::throwIfThisInitialized(FR frInput) {
  comment("// ThrowIfThisInitialized r%u", frInput.index());

  asmjit::Label slowPathLab = newSlowPathLabel();

  // We have to sync registers when the throw is inside a try region
  // because we could read from the FRs again in this function.
  // Outside a try it's not observable behavior.
  // Note that only the sync is needed, not a free. A free is required when a
  // call is emitted on a path that continues in this basic block, since temps
  // are caller-saved. Here the only call is the non-returning throw in the
  // out-of-line slow path; the fall-through path calls nothing, and the catch
  // handler begins a new basic block, which re-normalizes temp state anyway.
  // Freeing would only force needless reloads for the rest of the block.
  //
  // Cf. fastArrayLoad's #else (non-compressed-pointer) path, which syncs
  // without freeing for the same reason. Its
  // HERMESVM_COMPRESSED_POINTERS/HERMESVM_BOXED_DOUBLES path is not
  // comparable: it emits an unconditional runtime call, so it must sync and
  // free regardless of isInTry(). Cf. also throwInst, which syncs only under
  // isInTry() but frees unconditionally, because it emits its call inline.
  if (isInTry())
    syncAllFRTempExcept({});
  HWReg hwInput = getOrAllocFRInGpX(frInput, true);
  HWReg hwTemp = allocTempGpX();
  freeReg(hwTemp);

  emit_sh_ljs_is_empty(a, hwTemp.gpq(), hwInput.gpq());
  a.jne(slowPathLab);

  slowPaths_.emplace_back(
      slowPathLab, emittingIP, [frInput](Emitter &em, SlowPath &sp) {
        em.comment("// Slow path: ThrowIfThisInitialized r%u", frInput.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(x86::rdi, xRuntime);
        EMIT_RUNTIME_CALL(
            em, void (*)(SHRuntime *), _sh_throw_this_already_initialized);
        // Call does not return.
      });
}

void Emitter::jmpTrueFalse(
    bool onTrue,
    const asmjit::Label &target,
    FR frInput) {
  comment("// Jmp%s r%u", onTrue ? "True" : "False", frInput.index());

  // Do this always, since this could be the end of the BB.
  syncAllFRTempExcept(FR());

  if (isFRKnownType(frInput, FRType::Number)) {
    HWReg hwInput = getOrAllocFRInVecD(frInput, true);
    emitTypeAssert(frInput, hwInput, TypePred::IsNumber);
    // x86-64: arm64 needs two branches, on < 0 and on > 0, because its
    // fcmp reports "less" and "greater" separately and both are false on
    // unordered. vucomisd sets ZF for equal *and* for unordered, so a
    // truthy number is exactly ZF==0 -- one branch, with NaN falling out
    // for free.
    a.vucomisd(hwInput.xmm(), roConst64(0, "0.0"));
    a.j(onTrue ? x86::CondCode::kNE : x86::CondCode::kE, target);
  } else if (isFRKnownType(frInput, FRType::Bool)) {
    HWReg hwInput = getOrAllocFRInGpX(frInput, true);
    emitTypeAssert(frInput, hwInput, TypePred::IsBool);

    static_assert(
        HERMESVALUE_VERSION == 2, "bool is encoded as a bit at kHV_BoolBitIdx");
    // x86-64: kHV_BoolBitIdx is far above the 32 bits that a `test`
    // immediate can reach, so read the bit with `bt`, which takes an 8-bit
    // index and reports the bit in CF.
    a.bt(hwInput.gpq(), kHV_BoolBitIdx);
    a.j(onTrue ? x86::CondCode::kC : x86::CondCode::kNC, target);
  } else {
    // TODO: we should inline all of it.
    syncAllFRTempExcept({});
    // x86-64: the by-value SHLegacyValue argument goes in rdi.
    movHWFromFR(HWReg(x86::rdi), frInput);
    EMIT_RUNTIME_CALL(*this, bool (*)(SHLegacyValue), _sh_ljs_to_boolean);
    // x86-64: SysV defines only al for a bool return.
    a.test(x86::al, x86::al);
    a.j(onTrue ? x86::CondCode::kNZ : x86::CondCode::kZ, target);
    freeAllFRTempExcept(FR());
  }
}

void Emitter::jmp(const asmjit::Label &target) {
  comment("// Jmp Lx");
  // Do this always, since this could be the end of the BB.
  syncAllFRTempExcept(FR());
  freeAllFRTempExcept(FR());
  a.jmp(target);
}

void Emitter::jmpTypeOfIs(
    const asmjit::Label &target,
    FR frInput,
    TypeOfIsTypes origTypes) {
  comment("// jTypeOfIs r%u, %u", frInput.index(), origTypes.getRaw());

  TypeOfIsTypes invertedTypes = origTypes.invert();

  // Do this always because it's the end of a basic block.
  // The freeAllFRTempExcept calls are within fast paths because we may want to
  // use FR temps to syncToFrame(frInput) in the call path, and we know at JIT
  // time whether we'll emit the call path.
  syncAllFRTempExcept({});

  HWReg hwInput = getOrAllocFRInGpX(frInput, true);
  HWReg hwTemp = allocTempGpX();
  freeReg(hwTemp);
  freeAllFRTempExcept({});

  x86::Gp xInput = hwInput.gpq();
  x86::Gp xTemp = hwTemp.gpq();
  assert(xTemp != xInput && "the tag temp must differ from the input");

  // Try and see if inverting will result in fewer checks.
  // If so, flip it and set invert=true.
  bool invert = false;
  TypeOfIsTypes typesToCheck = origTypes;
  if (invertedTypes.count() < origTypes.count()) {
    invert = true;
    typesToCheck = invertedTypes;
  }

  // Nothing left to check means the answer does not depend on the input: an
  // empty origTypes matches nothing, so falling through is already right,
  // while an all-bits origTypes inverts to empty and matches everything, so
  // the branch is unconditional. None of the checks below are emitted.
  if (typesToCheck.count() == 0 && invert)
    a.jmp(target);

  // doneLab goes at the end of the instruction if there's multiple bits to
  // check, allowing short-circuiting the remaining checks if one of the
  // TypeOfIsTypes bits matches the kind of the input.
  // Use numRemainingTypes to track how many bits are left to check.
  asmjit::Label doneLab = a.newLabel();
  size_t numRemainingTypes = typesToCheck.count();

  // Checks are done as follows:
  // * If not inverted, just go to the target if the tag matches the bit,
  //   else fallthrough to the next case (if any).
  // * If inverted and there's multiple bits remaining,
  //   if the tag matches the bit, short circuit to doneLab and we've
  //   finished executing the instruction (no need to check the other bits).
  // * If inverted and there's only one bit remaining,
  //   then if the tag does NOT match the bit, go to the target
  //   immediately.
  //
  // In this way, single-bit checks (both inverted and not) are fast,
  // and multiple-bit checks are correct.
  // It's possible more complexity can optimize this further if needed, but this
  // is not a bad start.

  /// Emit the simple check for a match.
  /// If we're not inverted, branch to the target based on cond.
  /// If we're inverted:
  ///   If there's bits remaining to check, branch to doneLab if the tag matches
  ///   because we can short circuit the rest of the checks.
  ///   If there's no bits remaining to check, branch to the target if the tag
  ///   does NOT match the bit.
  /// \param cond the condition code, which if true, indicates a tag match.
  auto emitCondCheck = [this, invert, &numRemainingTypes, &target, &doneLab](
                           x86::CondCode cond) {
    if (!invert)
      a.j(cond, target);
    else if (numRemainingTypes > 0)
      a.j(cond, doneLab);
    else
      a.j(x86::negateCond(cond), target);
  };

  // x86-64: every tag helper below is a compare (or a shift plus a compare)
  // that leaves the answer in EFLAGS, exactly as on arm64, so the order of
  // check and branch is the same; any reorder that put another flag-writing
  // instruction between them would break.
  if (typesToCheck.hasUndefined()) {
    --numRemainingTypes;
    emit_sh_ljs_is_undefined(a, xTemp, xInput);
    emitCondCheck(x86::CondCode::kE);
  }
  if (typesToCheck.hasSymbol()) {
    --numRemainingTypes;
    emit_sh_ljs_is_symbol(a, xTemp, xInput);
    emitCondCheck(x86::CondCode::kE);
  }
  if (typesToCheck.hasString()) {
    --numRemainingTypes;
    emit_sh_ljs_is_string(a, xTemp, xInput);
    emitCondCheck(x86::CondCode::kE);
  }
  if (typesToCheck.hasBoolean()) {
    --numRemainingTypes;
    emit_sh_ljs_is_bool(a, xTemp, xInput);
    emitCondCheck(x86::CondCode::kE);
  }
  if (typesToCheck.hasNull()) {
    --numRemainingTypes;
    emit_sh_ljs_is_null(a, xTemp, xInput);
    emitCondCheck(x86::CondCode::kE);
  }
  if (typesToCheck.hasBigint()) {
    --numRemainingTypes;
    emit_sh_ljs_is_bigint(a, xTemp, xInput);
    emitCondCheck(x86::CondCode::kE);
  }
  if (typesToCheck.hasNumber()) {
    --numRemainingTypes;
    // x86-64: arm64 materializes the double limit and compares against it
    // inline; that is exactly emit_sh_ljs_is_double(), which takes its
    // operands the other way round (input first, then the temp it clobbers).
    static_assert(
        HERMESVALUE_VERSION == 2,
        "HVTag_First must be the first after double limit");
    emit_sh_ljs_is_double(a, xInput, xTemp);
    emitCondCheck(x86::CondCode::kB);
  }
  // TODO: Special-case if both hasObject() and hasFunction() are set,
  // because we no longer would need to check the CellKind.
  if (typesToCheck.hasObject()) {
    --numRemainingTypes;
    asmjit::Label objectDoneLab = a.newLabel();
    emit_sh_ljs_is_object(a, xTemp, xInput);
    if (!invert)
      a.jne(objectDoneLab);
    else if (numRemainingTypes > 0)
      a.jne(objectDoneLab);
    else
      a.jne(target);
    emit_sh_ljs_get_pointer(a, xTemp, xInput);
    emit_gccell_get_kind(a, xTemp, xTemp);
    emit_cellkind_in_range(
        a,
        xTemp,
        xTemp,
        CellKind::CallableKind_first,
        CellKind::CallableKind_last);
    emitCondCheck(x86::CondCode::kA);
    a.bind(objectDoneLab);
  }
  if (typesToCheck.hasFunction()) {
    --numRemainingTypes;
    asmjit::Label functionDoneLab = a.newLabel();
    emit_sh_ljs_is_object(a, xTemp, xInput);
    if (!invert)
      a.jne(functionDoneLab);
    else if (numRemainingTypes > 0)
      a.jne(functionDoneLab);
    else
      a.jne(target);
    emit_sh_ljs_get_pointer(a, xTemp, xInput);
    emit_gccell_get_kind(a, xTemp, xTemp);
    emit_cellkind_in_range(
        a,
        xTemp,
        xTemp,
        CellKind::CallableKind_first,
        CellKind::CallableKind_last);
    emitCondCheck(x86::CondCode::kBE);
    a.bind(functionDoneLab);
  }

  assert(numRemainingTypes == 0 && "missed a type");

  // Put doneLab after, so we skip the branch if we directly branch to doneLab
  // from above.
  a.bind(doneLab);
}

void Emitter::typeOfIs(FR frRes, FR frInput, TypeOfIsTypes origTypes) {
  comment(
      "// typeOfIs r%u, r%u, %u",
      frRes.index(),
      frInput.index(),
      origTypes.getRaw());

  // Store the input in hwInputTemp for the duration of the instruction.
  // Needed because it's possible frRes == frInput, and we want to write to
  // frRes at the top of the instruction.
  HWReg hwInputTemp;
  if (frRes == frInput) {
    hwInputTemp = allocTempGpX();
    movHWFromFR(hwInputTemp, frInput);
  } else {
    hwInputTemp = getOrAllocFRInGpX(frInput, true);
  }
  HWReg hwTemp = allocTempGpX();
  HWReg hwRes = getOrAllocFRInGpX(frRes, false);
  frUpdatedWithHW(frRes, hwRes);
  freeReg(hwTemp);
  if (frRes == frInput) {
    freeReg(hwInputTemp);
  }

  x86::Gp xInputTemp = hwInputTemp.gpq();
  x86::Gp xTemp = hwTemp.gpq();
  x86::Gp xRes = hwRes.gpq();
  // hwTemp is still allocated when hwRes is, so the two cannot alias. Both
  // emit_sh_ljs_is_undefined() and emit_sh_ljs_bool() rely on that.
  assert(xTemp != xInputTemp && "the tag temp must differ from the input");
  assert(xTemp != xRes && "the tag temp must differ from the result");

  TypeOfIsTypes invertedTypes = origTypes.invert();

  // Try and see if inverting will result in fewer checks.
  // If so, flip it and set invert=true.
  bool invert = false;
  TypeOfIsTypes typesToCheck = origTypes;
  if (invertedTypes.count() < origTypes.count()) {
    invert = true;
    typesToCheck = invertedTypes;
  }

  // Nothing left to check means the answer does not depend on the input: an
  // empty origTypes matches nothing, and an all-bits origTypes inverts to
  // empty and matches everything. Either way the result is the same constant
  // the individual cases produce when no tag can match, and none of the checks
  // below are emitted.
  if (typesToCheck.count() == 0)
    a.mov(xRes.r32(), asmjit::Imm(invert ? 1 : 0));

  // matchLab goes directly to the end of the instruction if there are multiple
  // bits to check, allowing short-circuiting the remaining checks if one of the
  // TypeOfIsTypes bits matches the kind of the input.
  // If there's only one bit to check, we don't put extra code the end - none of
  // the other cases will be emitted.
  asmjit::Label matchLab{};
  if (typesToCheck.count() > 1)
    matchLab = a.newLabel();

  // First, initialize xRes if necessary:
  // * If there are multiple bits set, initialize it to the value we would
  //   produce on a match. This is false if inverted and true otherwise.
  // * If there's only one bit set, leave it uninitialized, since we will
  //   overwrite the value in the individual cases with setcc.
  //
  // Checks are done as follows:
  // * If there are multiple bits set, then matchLab is valid,
  //   so if the tag matches the bit, branch to matchLab.
  //   If the tag doesn't match, then fall through to the next check.
  // * If there's only one bit set, then matchLab is NOT valid,
  //   so emit setcc with the appropriate condition code and we're done.
  //
  // In this way, single-bit checks (both inverted and not) are fast,
  // and multiple-bit checks are correct.

  /// Emit the simple check for a match.
  /// If there's multiple bits to check, this will branch based on \p cond
  /// to matchLab if the tag matches.
  /// If there's only one bit to check, this will emit a setcc with the
  /// appropriate condition code (and we're done).
  /// \param cond the condition code, which if true, indicates a tag match.
  ///
  /// x86-64: arm64's cset writes 0 or 1 into the whole 64-bit register. x86's
  /// setcc writes only a byte, so the zero-extension is explicit -- which is
  /// also what leaves the high bits clear for emit_sh_ljs_bool() below.
  auto emitCondCheck = [this, invert, &xRes, &matchLab](x86::CondCode cond) {
    if (matchLab.isValid()) {
      a.j(cond, matchLab);
    } else {
      a.set(!invert ? cond : x86::negateCond(cond), xRes.r8());
      a.movzx(xRes.r32(), xRes.r8());
    }
  };

  // As described above, if there are multiple cases, initialize it to the value
  // it should have on a successful match.
  //
  // x86-64: `mov` writes no EFLAGS -- unlike the `xor` that would otherwise be
  // the idiomatic way to produce a zero -- which is what lets the two
  // initializing movs inside the object and function cases below sit between a
  // tag comparison and the branch that reads it, exactly as on arm64. Any
  // reorder or substitution there is a bug.
  if (matchLab.isValid())
    a.mov(xRes.r32(), asmjit::Imm(invert ? 0 : 1));

  if (typesToCheck.hasUndefined()) {
    emit_sh_ljs_is_undefined(a, xTemp, xInputTemp);
    emitCondCheck(x86::CondCode::kE);
  }
  if (typesToCheck.hasSymbol()) {
    emit_sh_ljs_is_symbol(a, xTemp, xInputTemp);
    emitCondCheck(x86::CondCode::kE);
  }
  if (typesToCheck.hasString()) {
    emit_sh_ljs_is_string(a, xTemp, xInputTemp);
    emitCondCheck(x86::CondCode::kE);
  }
  if (typesToCheck.hasBoolean()) {
    emit_sh_ljs_is_bool(a, xTemp, xInputTemp);
    emitCondCheck(x86::CondCode::kE);
  }
  if (typesToCheck.hasBigint()) {
    emit_sh_ljs_is_bigint(a, xTemp, xInputTemp);
    emitCondCheck(x86::CondCode::kE);
  }
  if (typesToCheck.hasNull()) {
    emit_sh_ljs_is_null(a, xTemp, xInputTemp);
    emitCondCheck(x86::CondCode::kE);
  }
  if (typesToCheck.hasNumber()) {
    static_assert(
        HERMESVALUE_VERSION == 2,
        "HVTag_First must be the first after double limit");
    emit_sh_ljs_is_double(a, xInputTemp, xTemp);
    emitCondCheck(x86::CondCode::kB);
  }
  if (typesToCheck.hasObject()) {
    asmjit::Label objectDoneLab = a.newLabel();
    emit_sh_ljs_is_object(a, xTemp, xInputTemp);
    if (matchLab.isValid()) {
      // If the tag did NOT match, we can't run anything else in this case.
      // We must branch, jne and proceed to try matching any other cases.
      a.jne(objectDoneLab);
    } else {
      // No more tags to check. Decide the result here and go to the end.
      a.mov(xRes.r32(), asmjit::Imm(invert ? 1 : 0));
      a.jne(objectDoneLab);
    }
    emit_sh_ljs_get_pointer(a, xTemp, xInputTemp);
    emit_gccell_get_kind(a, xTemp, xTemp);
    emit_cellkind_in_range(
        a,
        xTemp,
        xTemp,
        CellKind::CallableKind_first,
        CellKind::CallableKind_last);
    emitCondCheck(x86::CondCode::kA);
    a.bind(objectDoneLab);
  }
  if (typesToCheck.hasFunction()) {
    asmjit::Label functionDoneLab = a.newLabel();
    emit_sh_ljs_is_object(a, xTemp, xInputTemp);
    if (matchLab.isValid()) {
      // If the tag did NOT match, we can't run anything else in this case.
      // We must branch, jne and proceed to try matching any other cases.
      a.jne(functionDoneLab);
    } else {
      // No more tags to check. Decide the result here and go to the end.
      a.mov(xRes.r32(), asmjit::Imm(invert ? 1 : 0));
      a.jne(functionDoneLab);
    }
    emit_sh_ljs_get_pointer(a, xTemp, xInputTemp);
    emit_gccell_get_kind(a, xTemp, xTemp);
    emit_cellkind_in_range(
        a,
        xTemp,
        xTemp,
        CellKind::CallableKind_first,
        CellKind::CallableKind_last);
    emitCondCheck(x86::CondCode::kBE);
    a.bind(functionDoneLab);
  }

  if (matchLab.isValid()) {
    // We failed to match, so flip the result
    a.xor_(xRes.r32(), asmjit::Imm(1));
    // We initialize xRes to the "match value", so there is nothing to do on a
    // match.
    a.bind(matchLab);
  }

  // xRes contains either 0 or 1 at this point, turn it into a bool HermesValue.
  emit_sh_ljs_bool(a, xRes, xTemp);
}

void Emitter::jmpUndefined(const asmjit::Label &target, FR frInput) {
  comment("// JmpUndefined r%u", frInput.index());

  // Do this always, since this could be the end of the BB.
  syncAllFRTempExcept(FR());
  freeAllFRTempExcept(FR());

  if (isFRKnownType(frInput, FRType::Number) ||
      isFRKnownType(frInput, FRType::Bool)) {
    emitTypeAssertFR(
        frInput,
        isFRKnownType(frInput, FRType::Number) ? TypePred::IsNumber
                                               : TypePred::IsBool);
    return;
  }

  HWReg hwInput = getOrAllocFRInGpX(frInput, true);
  HWReg hwTmpTag = allocTempGpX();

  emit_sh_ljs_is_undefined(a, hwTmpTag.gpq(), hwInput.gpq());
  a.je(target);

  freeReg(hwTmpTag);
}

void Emitter::jmpBuiltinIs(
    bool invert,
    const asmjit::Label &target,
    uint8_t builtinIndex,
    FR frInput) {
  comment(
      "// JmpBuiltinIs%s r%u, %u",
      invert ? "Not" : "",
      frInput.index(),
      builtinIndex);

  // Do this always, since this could be the end of the BB.
  syncAllFRTempExcept({});
  HWReg hwInput = getOrAllocFRInGpX(frInput, true);
  HWReg hwBuiltin = allocTempGpX();
  freeReg(hwBuiltin);
  freeAllFRTempExcept({});

  // Load builtin pointer.
  emit_load_builtin_closure(a, hwBuiltin.gpq(), builtinIndex);

  // Encode an object HermesValue. x86-64: this clobbers EFLAGS (see
  // emit_sh_ljs_object), which is why it must precede the compare below and
  // not the other way round.
  emit_sh_ljs_object(a, hwBuiltin.gpq());

  // Compare the builtin pointer with the input, branch.
  a.cmp(hwBuiltin.gpq(), hwInput.gpq());
  if (!invert)
    a.je(target);
  else
    a.jne(target);
}

void Emitter::jCond(
    bool forceNumber,
    bool invert,
    bool passArgsByVal,
    const asmjit::Label &target,
    FR frLeft,
    FR frRight,
    const char *name,
    x86::CondCode condCode,
    bool swapOperands,
    void *slowCall,
    const char *slowCallName) {
  comment(
      "// j_%s_%s Lx, r%u, r%u",
      invert ? "not" : "",
      name,
      frLeft.index(),
      frRight.index());
  HWReg hwLeft, hwRight;
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

  if (slow) {
    slowPathLab = newSlowPathLabel();
    contLab = newContLabel();
    syncToFrame(frLeft);
    syncToFrame(frRight);
  }
  // Do this always, since this could be the end of the BB.
  syncAllFRTempExcept(FR());

  hwLeft = getOrAllocFRInVecD(frLeft, true);
  hwRight = getOrAllocFRInVecD(frRight, true);

  if (leftIsNum)
    emitTypeAssert(frLeft, hwLeft, TypePred::IsNumber);
  if (rightIsNum)
    emitTypeAssert(frRight, hwRight, TypePred::IsNumber);

  // x86-64: "less" and "less or equal" compare the reversed operands, so
  // that the condition stays in the above family. See the condition mapping
  // in the design.
  a.vucomisd(
      swapOperands ? hwRight.xmm() : hwLeft.xmm(),
      swapOperands ? hwLeft.xmm() : hwRight.xmm());

  // x86-64: kA and kAE are false on unordered, exactly like the arm64 codes
  // this was ported from, so their branches may precede the unordered
  // routing. kE is not -- vucomisd sets ZF on unordered -- so its branch
  // must always come after unordered has been dealt with.
  bool const unorderedSafe = condCode != x86::CondCode::kE;

  // If the condition is not inverted, then it can only produce true if both
  // operands are numbers. Since we use NaN boxing, we know that all non-number
  // values will be NaN and therefore produce false. So if the result is true,
  // we can take the jump without checking for numbers.
  if (!invert && unorderedSafe)
    a.j(condCode, target);

  // Where there is a slow path, unordered means "not both numbers" and is
  // routed there. Where there is not, both operands are statically numbers,
  // but either can still be the JS NaN; only the equal family, whose
  // condition codes read ZF, has to do anything about that.
  asmjit::Label unorderedSkipLab;
  if (slow) {
    // Since HermesValue is NaN-boxed we know that all non-number values will be
    // NaN. So we can conveniently test for non-number values by checking for
    // NaN. x86-64: unordered is reported in PF.
    static_assert(HERMESVALUE_VERSION == 2, "Non-numbers must be NaN");
    a.jp(slowPathLab);
  } else if (!unorderedSafe) {
    if (invert) {
      // !(NaN == x) is true.
      a.jp(target);
    } else {
      // NaN == x is false: skip the branch below.
      unorderedSkipLab = a.newLabel();
      a.jp(unorderedSkipLab);
    }
  }

  // If the condition is inverted, it will produce true if one of the operands
  // is a NaN, so we can only check it after the slow path check, since it would
  // incorrectly be taken for non-numbers.
  if (invert)
    a.j(x86::negateCond(condCode), target);
  else if (!unorderedSafe)
    a.j(condCode, target);

  if (unorderedSkipLab.isValid())
    a.bind(unorderedSkipLab);

  if (!slow)
    return;

  a.bind(contLab);

  // Do this always, since this is the end of the BB.
  freeAllFRTempExcept(FR());

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [name,
       slowCall,
       slowCallName,
       target,
       frLeft,
       frRight,
       invert,
       passArgsByVal](Emitter &em, SlowPath &sp) {
        em.comment(
            "// Slow path: j_%s%s Lx, r%u, r%u",
            invert ? "not_" : "",
            name,
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
        // x86-64: SysV defines only al for a bool return.
        em.a.test(x86::al, x86::al);
        em.a.j(!invert ? x86::CondCode::kNZ : x86::CondCode::kZ, target);
        em.a.jmp(sp.contLab);
      });
}

void Emitter::jStrictEqual(
    bool invert,
    const asmjit::Label &target,
    FR frLeft,
    FR frRight) {
  comment(
      "// JStrict%sEq Lx, r%u, r%u",
      invert ? "Not" : "",
      frLeft.index(),
      frRight.index());

  // Fast path for raw-only comparison. One of the operands is a non-number
  // non-pointer value, meaning we can compare bits directly.
  if (isFRKnownBool(frLeft) || isFRKnownBool(frRight) ||
      isFRKnownOtherNonPtr(frLeft) || isFRKnownOtherNonPtr(frRight)) {
    // Do this always, since this could be the end of the BB.
    syncAllFRTempExcept({});
    HWReg hwLeft = getOrAllocFRInGpX(frLeft, true);
    HWReg hwRight = getOrAllocFRInGpX(frRight, true);
    freeAllFRTempExcept({});

    if (isFRKnownBool(frLeft) || isFRKnownOtherNonPtr(frLeft))
      emitTypeAssert(frLeft, hwLeft, TypePred::BitComparable);
    if (isFRKnownBool(frRight) || isFRKnownOtherNonPtr(frRight))
      emitTypeAssert(frRight, hwRight, TypePred::BitComparable);

    // An integer compare is never unordered, so kE/kNE are exact.
    a.cmp(hwLeft.gpq(), hwRight.gpq());
    a.j(!invert ? x86::CondCode::kE : x86::CondCode::kNE, target);
    return;
  }

  // Fast path for number (double) comparison.
  // One of the operands is a number, so there's two cases:
  // * It's NaN: it'll compare false against everything which is what we want.
  // * It's not NaN: It won't have NaN tag bits so it'll compare false against
  //   all non-double HVs and correctly compare true against the same number.
  if (isFRKnownNumber(frLeft) || isFRKnownNumber(frRight)) {
    // Do this always, since this could be the end of the BB.
    syncAllFRTempExcept(FR());
    HWReg hwLeftD = getOrAllocFRInVecD(frLeft, true);
    HWReg hwRightD = getOrAllocFRInVecD(frRight, true);
    freeAllFRTempExcept({});

    if (isFRKnownNumber(frLeft))
      emitTypeAssert(frLeft, hwLeftD, TypePred::IsNumber);
    if (isFRKnownNumber(frRight))
      emitTypeAssert(frRight, hwRightD, TypePred::IsNumber);

    a.vucomisd(hwLeftD.xmm(), hwRightD.xmm());
    // x86-64: unordered -- a JS NaN, or a NaN-boxed non-number against the
    // operand known to be a number -- means "not strictly equal", but ZF
    // says the opposite, so PF has to be consulted first.
    if (!invert) {
      asmjit::Label skipLab = a.newLabel();
      a.jp(skipLab);
      a.je(target);
      a.bind(skipLab);
    } else {
      a.jp(target);
      a.jne(target);
    }
    return;
  }

  // Do this always, since this could be the end of the BB.
  syncAllFRTempExcept(FR());

  HWReg hwLeftD = getOrAllocFRInVecD(frLeft, true);
  HWReg hwRightD = getOrAllocFRInVecD(frRight, true);

  HWReg hwTmpLeft = allocTempGpX();
  x86::Gp xTmpLeft = hwTmpLeft.gpq();
  HWReg hwTmpRight = allocTempGpX();
  x86::Gp xTmpRight = hwTmpRight.gpq();
  HWReg hwLeft = getOrAllocFRInGpX(frLeft, true);
  HWReg hwRight = getOrAllocFRInGpX(frRight, true);
  freeReg(hwTmpLeft);
  freeReg(hwTmpRight);

  // Label for non-number comparisons.
  auto nonNumberLab = a.newLabel();

  // Set up slow path.
  auto slowPathLab = newSlowPathLabel();
  auto contLab = newContLabel();
  syncToFrame(frLeft);
  syncToFrame(frRight);
  freeAllFRTempExcept(FR());

  a.vucomisd(hwLeftD.xmm(), hwRightD.xmm());
  // Since HermesValue is NaN-boxed we know that all non-number values will be
  // NaN. So we can conveniently test for non-number values by checking for
  // NaN. x86-64: unordered is reported in PF -- and, unlike arm64's kEQ,
  // x86's kE is *true* on unordered, so the equality branch cannot precede
  // this routing; it follows it instead.
  static_assert(HERMESVALUE_VERSION == 2, "Non-numbers must be NaN");
  a.jp(nonNumberLab);
  // Neither operand is NaN, so we can jump to the correct endpoint: equality
  // here is real equality, and inequality is real inequality.
  // If the branch fails, we go to contLab.
  a.j(!invert ? x86::CondCode::kE : x86::CondCode::kNE, target);
  a.jmp(contLab);

  // Convenience labels so we don't have to think too hard about inverted logic
  // below.
  const asmjit::Label &equalLab = !invert ? target : contLab;
  const asmjit::Label &notEqualLab = !invert ? contLab : target;

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

  a.bind(contLab);

  slowPaths_.emplace_back(
      slowPathLab,
      contLab,
      emittingIP,
      [target, frLeft, frRight, invert](Emitter &em, SlowPath &sp) {
        em.comment(
            "// Slow path: %s Lx, r%u, r%u",
            invert ? "j_strict_not_eq" : "j_strict_eq",
            frLeft.index(),
            frRight.index());
        em.a.bind(sp.slowPathLab);
        em._loadFrame(HWReg(x86::rdi), frLeft);
        em._loadFrame(HWReg(x86::rsi), frRight);
        em.callRuntimeWithSavedIP(
            (void *)_sh_ljs_strict_equal, "_sh_ljs_strict_equal");
        // x86-64: SysV defines only al for a bool return.
        em.a.test(x86::al, x86::al);
        em.a.j(!invert ? x86::CondCode::kNZ : x86::CondCode::kZ, target);
        em.a.jmp(sp.contLab);
      });
}

} // namespace hermes::vm::x86_64

#endif // HERMESVM_JIT_X86_64

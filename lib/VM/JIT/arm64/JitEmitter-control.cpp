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

#include "hermes/VM/CellKind.h"

namespace hermes::vm::arm64 {

void Emitter::catchInst(FR frRes) {
  comment("// Catch r%u", frRes.index());

  HWReg hwTemp = allocTempGpX();
  HWReg hwRes = getOrAllocFRInGpX(frRes, false);
  frUpdatedWithHW(frRes, hwRes);
  freeReg(hwTemp);

  // Catch simply returns the thrown value and clears it.

  // Read thrown value.
  a.ldr(hwRes.a64GpX(), a64::Mem(xRuntime, RuntimeOffsets::thrownValue));
  // Clear thrown value.
  loadBits64InGp(hwTemp.a64GpX(), _sh_ljs_empty().raw, "empty");
  a.str(hwTemp.a64GpX(), a64::Mem(xRuntime, RuntimeOffsets::thrownValue));
}

void Emitter::ret(FR frValue) {
  movHWFromFR(HWReg::gpX(21), frValue);
  a.b(returnLabel_);
}

void Emitter::throwInst(FR frInput) {
  comment("// Throw r%u", frInput.index());

  // We have to sync registers when the throw is inside a try region
  // because we could read from the FRs again in this function.
  if (isInTry())
    syncAllFRTempExcept({});
  movHWFromFR(HWReg::gpX(1), frInput);
  freeAllFRTempExcept({});

  a.mov(a64::x0, xRuntime);
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
    emit_sh_ljs_is_empty(a, hwTemp.a64GpX(), hwInput.a64GpX());
  else
    emit_sh_ljs_is_undefined(a, hwTemp.a64GpX(), hwInput.a64GpX());
  a.b_eq(slowPathLab);

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
        em.a.mov(a64::x0, xRuntime);
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

  emit_sh_ljs_is_empty(a, hwTemp.a64GpX(), hwInput.a64GpX());
  a.b_ne(slowPathLab);

  slowPaths_.emplace_back(
      slowPathLab, emittingIP, [frInput](Emitter &em, SlowPath &sp) {
        em.comment("// Slow path: ThrowIfThisInitialized r%u", frInput.index());
        em.a.bind(sp.slowPathLab);
        em.a.mov(a64::x0, xRuntime);
        EMIT_RUNTIME_CALL(
            em, void (*)(SHRuntime *), _sh_throw_this_already_initialized);
        // Call does not return.
      });
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

  auto xInput = hwInput.a64GpX();
  auto xTemp = hwTemp.a64GpX();
  auto wTemp = xTemp.w();

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
    a.b(target);

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
                           a64::CondCode cond) {
    if (!invert)
      a.b(cond, target);
    else if (numRemainingTypes > 0)
      a.b(cond, doneLab);
    else
      a.b(a64::negateCond(cond), target);
  };

  if (typesToCheck.hasUndefined()) {
    --numRemainingTypes;
    emit_sh_ljs_is_undefined(a, xTemp, xInput);
    emitCondCheck(a64::CondCode::kEQ);
  }
  if (typesToCheck.hasSymbol()) {
    --numRemainingTypes;
    emit_sh_ljs_is_symbol(a, xTemp, xInput);
    emitCondCheck(a64::CondCode::kEQ);
  }
  if (typesToCheck.hasString()) {
    --numRemainingTypes;
    emit_sh_ljs_is_string(a, xTemp, xInput);
    emitCondCheck(a64::CondCode::kEQ);
  }
  if (typesToCheck.hasBoolean()) {
    --numRemainingTypes;
    emit_sh_ljs_is_bool(a, xTemp, xInput);
    emitCondCheck(a64::CondCode::kEQ);
  }
  if (typesToCheck.hasNull()) {
    --numRemainingTypes;
    emit_sh_ljs_is_null(a, xTemp, xInput);
    emitCondCheck(a64::CondCode::kEQ);
  }
  if (typesToCheck.hasBigint()) {
    --numRemainingTypes;
    emit_sh_ljs_is_bigint(a, xTemp, xInput);
    emitCondCheck(a64::CondCode::kEQ);
  }
  if (typesToCheck.hasNumber()) {
    --numRemainingTypes;
    static_assert(
        HERMESVALUE_VERSION == 2,
        "HVTag_First must be the first after double limit");
    loadBits64InGp(
        xTemp, ((uint64_t)HVTag_First << kHV_NumDataBits), "doubleLim");
    a.cmp(xInput, xTemp);
    emitCondCheck(a64::CondCode::kLO);
  }
  // TODO: Special-case if both hasObject() and hasFunction() are set,
  // because we no longer would need to check the CellKind.
  if (typesToCheck.hasObject()) {
    --numRemainingTypes;
    asmjit::Label objectDoneLab = a.newLabel();
    emit_sh_ljs_is_object(a, xTemp, xInput);
    if (!invert)
      a.b_ne(objectDoneLab);
    else if (numRemainingTypes > 0)
      a.b_ne(objectDoneLab);
    else
      a.b_ne(target);
    emit_sh_ljs_get_pointer(a, hwTemp.a64GpX(), hwInput.a64GpX());
    emit_gccell_get_kind(a, xTemp, xTemp);
    emit_cellkind_in_range(
        a,
        wTemp,
        wTemp,
        CellKind::CallableKind_first,
        CellKind::CallableKind_last);
    emitCondCheck(a64::CondCode::kHI);
    a.bind(objectDoneLab);
  }
  if (typesToCheck.hasFunction()) {
    --numRemainingTypes;
    asmjit::Label functionDoneLab = a.newLabel();
    emit_sh_ljs_is_object(a, xTemp, xInput);
    if (!invert)
      a.b_ne(functionDoneLab);
    else if (numRemainingTypes > 0)
      a.b_ne(functionDoneLab);
    else
      a.b_ne(target);
    emit_sh_ljs_get_pointer(a, xTemp, xInput);
    emit_gccell_get_kind(a, xTemp, xTemp);
    emit_cellkind_in_range(
        a,
        wTemp,
        wTemp,
        CellKind::CallableKind_first,
        CellKind::CallableKind_last);
    emitCondCheck(a64::CondCode::kLS);
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

  auto xInputTemp = hwInputTemp.a64GpX();
  auto xTemp = hwTemp.a64GpX();
  auto wTemp = xTemp.w();
  auto xRes = hwRes.a64GpX();

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
    a.mov(xRes, invert ? 1 : 0);

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
  //   overwrite the value in the individual cases with cset.
  //
  // Checks are done as follows:
  // * If there are multiple bits set, then matchLab is valid,
  //   so if the tag matches the bit, branch to matchLab.
  //   If the tag doesn't match, then fall through to the next check.
  // * If there's only one bit set, then matchLab is NOT valid,
  //   so emit cset with the appropriate condition code and we're done.
  //
  // In this way, single-bit checks (both inverted and not) are fast,
  // and multiple-bit checks are correct.

  /// Emit the simple check for a match.
  /// If there's multiple bits to check, this will branch based on \p cond
  /// to matchLab if the tag matches.
  /// If there's only one bit to check, this will emit a cinc with the
  /// appropriate condition code (and we're done).
  /// \param cond the condition code, which if true, indicates a tag match.
  auto emitCondCheck = [this, invert, &xRes, &matchLab](a64::CondCode cond) {
    if (matchLab.isValid())
      a.b(cond, matchLab);
    else
      a.cset(xRes, !invert ? cond : a64::negateCond(cond));
  };

  // As described above, if there are multiple cases, initialize it to the value
  // it should have on a successful match.
  if (matchLab.isValid())
    a.mov(xRes, invert ? 0 : 1);

  if (typesToCheck.hasUndefined()) {
    emit_sh_ljs_is_undefined(a, xTemp, xInputTemp);
    emitCondCheck(a64::CondCode::kEQ);
  }
  if (typesToCheck.hasSymbol()) {
    emit_sh_ljs_is_symbol(a, xTemp, xInputTemp);
    emitCondCheck(a64::CondCode::kEQ);
  }
  if (typesToCheck.hasString()) {
    emit_sh_ljs_is_string(a, xTemp, xInputTemp);
    emitCondCheck(a64::CondCode::kEQ);
  }
  if (typesToCheck.hasBoolean()) {
    emit_sh_ljs_is_bool(a, xTemp, xInputTemp);
    emitCondCheck(a64::CondCode::kEQ);
  }
  if (typesToCheck.hasBigint()) {
    emit_sh_ljs_is_bigint(a, xTemp, xInputTemp);
    emitCondCheck(a64::CondCode::kEQ);
  }
  if (typesToCheck.hasNull()) {
    emit_sh_ljs_is_null(a, xTemp, xInputTemp);
    emitCondCheck(a64::CondCode::kEQ);
  }
  if (typesToCheck.hasNumber()) {
    static_assert(
        HERMESVALUE_VERSION == 2,
        "HVTag_First must be the first after double limit");
    loadBits64InGp(
        xTemp, ((uint64_t)HVTag_First << kHV_NumDataBits), "doubleLim");
    a.cmp(xInputTemp, xTemp);
    emitCondCheck(a64::CondCode::kLO);
  }
  if (typesToCheck.hasObject()) {
    asmjit::Label objectDoneLab = a.newLabel();
    emit_sh_ljs_is_object(a, xTemp, xInputTemp);
    if (matchLab.isValid()) {
      // If the tag did NOT match, we can't run anything else in this case.
      // We must branch, b_ne and proceed to try matching any other cases.
      a.b_ne(objectDoneLab);
    } else {
      // No more tags to check. Decide the result here and go to the end.
      a.mov(xRes, invert ? 1 : 0);
      a.b_ne(objectDoneLab);
    }
    emit_sh_ljs_get_pointer(a, xTemp, xInputTemp);
    emit_gccell_get_kind(a, xTemp, xTemp);
    emit_cellkind_in_range(
        a,
        wTemp,
        wTemp,
        CellKind::CallableKind_first,
        CellKind::CallableKind_last);
    emitCondCheck(a64::CondCode::kHI);
    a.bind(objectDoneLab);
  }
  if (typesToCheck.hasFunction()) {
    asmjit::Label functionDoneLab = a.newLabel();
    emit_sh_ljs_is_object(a, xTemp, xInputTemp);
    if (matchLab.isValid()) {
      // If the tag did NOT match, we can't run anything else in this case.
      // We must branch, b_ne and proceed to try matching any other cases.
      a.b_ne(functionDoneLab);
    } else {
      // No more tags to check. Decide the result here and go to the end.
      a.mov(xRes, invert ? 1 : 0);
      a.b_ne(functionDoneLab);
    }
    emit_sh_ljs_get_pointer(a, xTemp, xInputTemp);
    emit_gccell_get_kind(a, xTemp, xTemp);
    emit_cellkind_in_range(
        a,
        wTemp,
        wTemp,
        CellKind::CallableKind_first,
        CellKind::CallableKind_last);
    emitCondCheck(a64::CondCode::kLS);
    a.bind(functionDoneLab);
  }

  if (matchLab.isValid()) {
    // We failed to match, so flip the result
    a.eor(xRes, xRes, 1);
    // We initialize xRes to the "match value", so there is nothing to do on a
    // match.
    a.bind(matchLab);
  }

  // xRes contains either 0 or 1 at this point, turn it into a bool HermesValue.
  emit_sh_ljs_bool(a, xRes);
}

void Emitter::uintSwitchImm(
    FR frInput,
    const asmjit::Label &defaultLabel,
    llvh::ArrayRef<const asmjit::Label *> labels,
    uint32_t minVal,
    uint32_t maxVal) {
  comment(
      "// uintSwitchImm r%u, min %u, max %u", frInput.index(), minVal, maxVal);

  // minVal is compared against and subtracted below; both are add/sub
  // immediate forms with the same encoding limit.
  const bool minValIsImm = a64::Utils::isAddSubImm(minVal);

  // End of the basic block.
  syncAllFRTempExcept({});

  // Load the input value into a double register to check if it's an int.
  HWReg hwInput = getOrAllocFRInVecD(frInput, true);

  HWReg hwTempInput = allocTempGpX();
  HWReg hwTempTarget = allocTempGpX();
  HWReg hwTempD = allocTempVecD();
  freeReg(hwTempInput);
  freeReg(hwTempTarget);
  freeReg(hwTempD);

  a64::VecD dInput = hwInput.a64VecD();
  a64::GpW wTempInput = hwTempInput.a64GpX().w();

  // Convert the input to an integer and back to double,
  // and check if the value remained the same.
  // If it didn't, jump to the default label.
  emit_double_is_uint32(a, wTempInput, hwTempD.a64VecD(), dInput);
  a.b_ne(defaultLabel);

  // Check if the integer value in xTemp is in range.
  // First check minVal.
  if (minValIsImm) {
    a.cmp(wTempInput, minVal);
  } else {
    a.mov(hwTempTarget.a64GpX().w(), minVal);
    a.cmp(wTempInput, hwTempTarget.a64GpX().w());
  }
  // If the value is lower than minVal, jump to the default label.
  a.b_lo(defaultLabel);

  // Now check maxVal.
  if (a64::Utils::isAddSubImm(maxVal)) {
    a.cmp(wTempInput, maxVal);
  } else {
    a.mov(hwTempTarget.a64GpX().w(), maxVal);
    a.cmp(wTempInput, hwTempTarget.a64GpX().w());
  }
  // If the value is higher than maxVal, jump to the default label.
  a.b_hi(defaultLabel);

  // Compute the offset into the jump table, dereference, and jump.
  // Offset by the minVal if necessary.
  if (minVal != 0) {
    if (minValIsImm) {
      a.sub(wTempInput, wTempInput, minVal);
    } else {
      a.mov(hwTempTarget.a64GpX().w(), minVal);
      a.sub(wTempInput, wTempInput, hwTempTarget.a64GpX().w());
    }
  }

  // Label for the start of the jump table and the base of the br instruction
  // that actually executes the switch.
  // Used for both purposes due to placement of the jump table directly after
  // the br.
  asmjit::Label tableLab = a.newLabel();

  // wTempInput contains the index into the jump table.
  a64::GpX xTempTarget = hwTempTarget.a64GpX();
  // Load the jump offset into wTempInput by using adr to find the address of
  // the table and then reading 4 bytes from an offset of wTempInput bytes.
  a.adr(xTempTarget, tableLab);
  // Left shift 2 to get the byte offset into the table.
  a.ldr(
      wTempInput,
      a64::Mem(xTempTarget, wTempInput, a64::Shift(a64::ShiftOp::kLSL, 2)));
  // Add the jump offset to the base of the table to get the target address.
  a.add(xTempTarget, xTempTarget, wTempInput.x(), a64::sxtw(0));
  // Branch to the target address.
  a.br(xTempTarget);

  // Emit the jump table.
  // NOTE: The jump table is emitted immediately after the br instruction that
  // uses it.
  a.bind(tableLab);
  for (const asmjit::Label *label : labels) {
    a.embedLabelDelta(*label, tableLab, /* size */ 4);
  }

  // Do this always, since this could be the end of the BB.
  freeAllFRTempExcept({});
}

void Emitter::stringSwitchImm(
    FR frInput,
    RuntimeModule *runtimeModule,
    uint32_t tableIndex,
    const asmjit::Label &defaultLabel,
    llvh::ArrayRef<const asmjit::Label *> caseLabels) {
  comment(
      "// stringSwitchImm r%u, size %zu", frInput.index(), caseLabels.size());

  // End of the basic block.
  syncAllFRTempExcept({});
  // The handler reads the value through the frame address passed below, so
  // the slot has to hold the current value.
  syncToFrame(frInput);

  a.mov(a64::x0, (uint64_t)runtimeModule);
  a.mov(a64::w1, tableIndex);
  loadFrameAddr(a64::x2, frInput);

  EMIT_RUNTIME_CALL_WITHOUT_SAVED_IP(
      *this,
      int64_t (*)(RuntimeModule *, uint32_t, SHLegacyValue *),
      _jit_string_switch_imm_table_lookup);

  // The lookup returns a case index, or a negative value when the operand is
  // not a string, or is a string that no case matches.
  a.cmp(a64::x0, 0);
  a.b_lt(defaultLabel);

  // The index selects a slot in the jump table below, which belongs to this
  // body alone. The shared runtime table maps strings to case indices only,
  // so a switch executed in an older version of this function still lands in
  // that older version's code. See _jit_string_switch_imm_table_lookup().
  //
  // The table holds deltas from its own start, and the adr that materializes
  // that start doubles as the base of the br, exactly as in uintSwitchImm().
  asmjit::Label tableLab = a.newLabel();

  // x1 and w2 are dead here: they are argument registers this sequence itself
  // clobbered above.
  a.adr(a64::x1, tableLab);
  a.ldr(a64::w2, a64::Mem(a64::x1, a64::x0, a64::Shift(a64::ShiftOp::kLSL, 2)));
  a.add(a64::x1, a64::x1, a64::x2, a64::sxtw(0));
  a.br(a64::x1);

  // Emit the jump table. As in uintSwitchImm(), it goes immediately after the
  // br that reads it; nothing falls into it.
  a.bind(tableLab);
  for (const asmjit::Label *label : caseLabels) {
    a.embedLabelDelta(*label, tableLab, /* size */ 4);
  }

  // Do this always, since this could be the end of the BB.
  freeAllFRTempExcept({});
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
    a.fcmp(hwInput.a64VecD(), 0.0);
    if (onTrue) {
      // Branch on < 0 and > 0. All that remains is 0 and NaN.
      a.b_mi(target);
      a.b_gt(target);
    } else {
      asmjit::Label label = a.newLabel();
      a.b_mi(label);
      a.b_gt(label);
      a.b(target);
      a.bind(label);
    }
  } else if (isFRKnownType(frInput, FRType::Bool)) {
    HWReg hwInput = getOrAllocFRInGpX(frInput, true);
    a64::GpX xInput = hwInput.a64GpX();
    emitTypeAssert(frInput, hwInput, TypePred::IsBool);

    static_assert(
        HERMESVALUE_VERSION == 2, "bool is encoded as a bit at kHV_BoolBitIdx");
    // We don't use tbz/tbnz here because they have a very restricted range.
    a.tst(xInput, 1ull << kHV_BoolBitIdx);
    a.b(onTrue ? a64::CondCode::kNotZero : a64::CondCode::kZero, target);
  } else {
    // TODO: we should inline all of it.
    syncAllFRTempExcept({});
    movHWFromFR(HWReg::gpX(0), frInput);
    EMIT_RUNTIME_CALL(*this, bool (*)(SHLegacyValue), _sh_ljs_to_boolean);
    if (onTrue)
      a.cbnz(a64::w0, target);
    else
      a.cbz(a64::w0, target);
    freeAllFRTempExcept(FR());
  }
}

void Emitter::jmp(const asmjit::Label &target) {
  comment("// Jmp Lx");
  // Do this always, since this could be the end of the BB.
  syncAllFRTempExcept(FR());
  freeAllFRTempExcept(FR());
  a.b(target);
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
  a64::GpX xInput = hwInput.a64GpX();
  HWReg hwTmpTag = allocTempGpX();
  a64::GpX xTmpTag = hwTmpTag.a64GpX();

  emit_sh_ljs_is_undefined(a, xTmpTag, xInput);
  a.b_eq(target);

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
  emit_load_builtin_closure(a, hwBuiltin.a64GpX(), builtinIndex);

  // Encode an object HermesValue.
  emit_sh_ljs_object(a, hwBuiltin.a64GpX());

  // Compare the builtin pointer with the input, branch.
  a.cmp(hwBuiltin.a64GpX(), hwInput.a64GpX());
  if (!invert)
    a.b_eq(target);
  else
    a.b_ne(target);
}

void Emitter::jCond(
    bool forceNumber,
    bool invert,
    bool passArgsByVal,
    const asmjit::Label &target,
    FR frLeft,
    FR frRight,
    const char *name,
    a64::CondCode condCode,
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

  a.fcmp(hwLeft.a64VecD(), hwRight.a64VecD());

  // If the condition is not inverted, then it can only produce true if both
  // operands are numbers. Since we use NaN boxing, we know that all non-number
  // values will be NaN and therefore produce false. So if the result is true,
  // we can take the jump without checking for numbers.
  if (!invert)
    a.b(condCode, target);

  if (slow) {
    // Since HermesValue is NaN-boxed we know that all non-number values will be
    // NaN. So we can conveniently test for non-number values by checking for
    // NaN. We can do that with the VS condition code, which is set if either
    // operand to fcmp is NaN.
    static_assert(HERMESVALUE_VERSION == 2, "Non-numbers must be NaN");
    a.b_vs(slowPathLab);
  }

  // If the condition is inverted, it will produce true if one of the operands
  // is a NaN, so we can only check it after the slow path check, since it would
  // incorrectly be taken for non-numbers.
  if (invert)
    a.b(a64::negateCond(condCode), target);

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
          em._loadFrame(HWReg::gpX(0), frLeft);
          em._loadFrame(HWReg::gpX(1), frRight);
        } else {
          em.a.mov(a64::x0, xRuntime);
          em.loadFrameAddr(a64::x1, frLeft);
          em.loadFrameAddr(a64::x2, frRight);
        }
        em.callRuntimeWithSavedIP(slowCall, slowCallName);
        if (!invert)
          em.a.cbnz(a64::w0, target);
        else
          em.a.cbz(a64::w0, target);
        em.a.b(sp.contLab);
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

    a.cmp(hwLeft.a64GpX(), hwRight.a64GpX());
    a.b(!invert ? a64::CondCode::kEQ : a64::CondCode::kNE, target);
    return;
  }

  // Fast path for number (double) comparison.
  // One of the operands is a number, so there's two cases:
  // * It's NaN: it'll fcmp false against everything which is what we want.
  // * It's not NaN: It won't have NaN tag bits so it'll compare false against
  //   all non-double HVs and correctly fcmp true against the same number.
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

    a.fcmp(hwLeftD.a64VecD(), hwRightD.a64VecD());
    a.b(!invert ? a64::CondCode::kEQ : a64::CondCode::kNE, target);
    return;
  }

  // Do this always, since this could be the end of the BB.
  syncAllFRTempExcept(FR());

  HWReg hwLeftD = getOrAllocFRInVecD(frLeft, true);
  HWReg hwRightD = getOrAllocFRInVecD(frRight, true);

  HWReg hwTmpLeft = allocTempGpX();
  a64::GpX xTmpLeft = hwTmpLeft.a64GpX();
  HWReg hwTmpRight = allocTempGpX();
  a64::GpX xTmpRight = hwTmpRight.a64GpX();
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

  a.fcmp(hwLeftD.a64VecD(), hwRightD.a64VecD());
  // If not inverted then equality here is real equality.
  // If the equality check fails, we don't know anything.
  if (!invert)
    a.b_eq(target);
  // Since HermesValue is NaN-boxed we know that all non-number values will be
  // NaN. So we can conveniently test for non-number values by checking for
  // NaN. We can do that with the VS condition code, which is set if either
  // operand to fcmp is NaN.
  static_assert(HERMESVALUE_VERSION == 2, "Non-numbers must be NaN");
  a.b_vs(nonNumberLab);
  // If neither number is NaN, then we can just jump to the correct endpoint.
  // We already checked equality above for the non-inverted case,
  // so just check the inverted case here to know if the values are not equal.
  // If all that fails, then the branch failed and we go to contLab.
  if (invert)
    a.b_ne(target);
  a.b(contLab);

  // Convenience labels so we don't have to think too hard about inverted logic
  // below.
  const asmjit::Label &equalLab = !invert ? target : contLab;
  const asmjit::Label &notEqualLab = !invert ? contLab : target;

  a.bind(nonNumberLab);
  emit_sh_ljs_is_double(a, hwLeft.a64GpX(), xTmpLeft);
  // Left is actually the JS NaN, which is never equal to anything.
  // No need to check RHS for JS NaN, because it won't cause false positive
  // on the raw bit check below.
  a.b_lo(notEqualLab);

  // Compare bits directly.
  // If they match exactly, the two values are equal.
  a.cmp(hwLeft.a64GpX(), hwRight.a64GpX());
  a.b_eq(equalLab);

  // First compare the tags. If they don't match, the two values are NOT equal.
  emit_sh_ljs_get_tag(a, xTmpLeft, hwLeft.a64GpX());
  emit_sh_ljs_get_tag(a, xTmpRight, hwRight.a64GpX());
  a.cmp(xTmpLeft, xTmpRight);
  a.b_ne(notEqualLab);

  // If the LHS is either a non-pointer or an object, we can compare raw values
  // only. We've already checked and we know that the raw values are not the
  // same, so if this is a non-pointer or an object, then the two values are NOT
  // strictly equal.
  emit_sh_ljs_tag_is_pointer(a, xTmpLeft);
  a.b_lo(notEqualLab);
  emit_sh_ljs_tag_is_object(a, xTmpLeft);
  a.b_eq(notEqualLab);

  // Now we know that the LHS is a non-object pointer.

  // Fast string path: string inequality can be easily determined by checking
  // the lengths. If the LHS isn't a string, go to slow path.
  emit_sh_ljs_tag_is_string(a, xTmpLeft);
  a.b_ne(slowPathLab);

  emit_stringprim_get_length_and_flags(a, xTmpLeft, hwLeft.a64GpX());
  emit_stringprim_get_length_and_flags(a, xTmpRight, hwRight.a64GpX());
  // XOR the lengths together and mask the result.
  // xTmpLeft will be nonzero if the lengths don't match.
  a.eor(xTmpLeft, xTmpLeft, xTmpRight);
  a.and_(xTmpLeft, xTmpLeft, RuntimeOffsets::stringPrimitiveLengthMask);
  // Length mismatch means we're done, the two values are NOT equal.
  a.cbnz(xTmpLeft, notEqualLab);
  a.b(slowPathLab);

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
        em._loadFrame(HWReg::gpX(0), frLeft);
        em._loadFrame(HWReg::gpX(1), frRight);
        em.callRuntimeWithSavedIP(
            (void *)_sh_ljs_strict_equal, "_sh_ljs_strict_equal");
        if (!invert)
          em.a.cbnz(a64::w0, target);
        else
          em.a.cbz(a64::w0, target);
        em.a.b(sp.contLab);
      });
}

} // namespace hermes::vm::arm64
#endif // HERMESVM_JIT_ARM64

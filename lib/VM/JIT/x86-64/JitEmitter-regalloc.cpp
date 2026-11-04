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

#include "llvh/ADT/STLExtras.h"

namespace hermes::vm::x86_64 {

void Emitter::_storeHWToFrame(FR fr, HWReg src) {
  _storeFrame(src, fr);
  frameRegs_[fr.index()].frameUpToDate = true;
}

void Emitter::movHWFromFR(HWReg hwRes, FR src) {
  FRState &frState = frameRegs_[src.index()];
  assert(!frState.regIsDirty && "Any local should have a valid value");
  if (frState.localGpX)
    movHWFromHW<true>(hwRes, frState.localGpX);
  else if (frState.localVecD)
    movHWFromHW<true>(hwRes, frState.localVecD);
  else if (frState.globalReg && frState.globalRegUpToDate)
    movHWFromHW<true>(hwRes, frState.globalReg);
  else
    _loadFrame(useReg(hwRes), src);
}

void Emitter::movHWFromMem(HWReg hwRes, x86::Mem src) {
  if (hwRes.isVecD())
    a.vmovsd(hwRes.xmm(), src);
  else
    a.mov(hwRes.gpq(), src);
}

void Emitter::movFRFromHW(FR dst, HWReg src, FRType type) {
  FRState &frState = frameRegs_[dst.index()];
  // If it is a local or global register, move the value into it and mark it as
  // updated.
  if (frState.localGpX) {
    movHWFromHW<false>(frState.localGpX, src);
    frUpdatedWithHW(dst, frState.localGpX, type);
  } else if (frState.localVecD) {
    movHWFromHW<false>(frState.localVecD, src);
    frUpdatedWithHW(dst, frState.localVecD, type);
  } else if (frState.globalReg) {
    movHWFromHW<false>(frState.globalReg, src);
    frUpdatedWithHW(dst, frState.globalReg, type);
  } else {
    // Otherwise store it directly to the frame.
    // This branch is reached only when the FR has no global register, so
    // the call can never record anything today. It is here for the
    // globalType/globalReg equivalence assert, and so that the path stays
    // covered if that ever changes.
    if (LLVM_UNLIKELY(emitTypeAsserts_))
      recordFRWriteForAssert(dst);
    _storeHWToFrame(dst, src);
    frUpdateType(dst, type);
    frState.frameUpToDate = true;
  }
}

void Emitter::syncFrameOutParam(FR fr, FRType type) {
  auto &frState = frameRegs_[fr.index()];

  if (LLVM_UNLIKELY(emitTypeAsserts_))
    recordFRWriteForAssert(fr);

  frState.frameUpToDate = true;

  // Since the frame is the source-of-truth here, there should not be any local
  // register.
  assert(!frState.localGpX && !frState.localVecD);

  if (frState.globalReg) {
    frState.globalRegUpToDate = true;
    _loadFrame(frState.globalReg, fr);
  }
  frUpdateType(fr, type);
}

void Emitter::freeReg(HWReg hwReg) {
  if (!hwReg.isValid())
    return;

  FR fr = hwRegs_[hwReg.combinedIndex()].contains;
  hwRegs_[hwReg.combinedIndex()].contains = {};

  if (hwReg.isGpX()) {
    if (fr.isValid()) {
      comment("    ; free r%u (r%u)", hwReg.indexInClass(), fr.index());
      assert(frameRegs_[fr.index()].localGpX == hwReg);
      frameRegs_[fr.index()].localGpX = {};
    } else {
      comment("    ; free r%u", hwReg.indexInClass());
    }
    if (isTempGpX(hwReg))
      gpTemp_.free(hwReg.indexInClass());
  } else {
    if (fr.isValid()) {
      comment("    ; free xmm%u (r%u)", hwReg.indexInClass(), fr.index());
      assert(frameRegs_[fr.index()].localVecD == hwReg);
      frameRegs_[fr.index()].localVecD = {};
    } else {
      comment("    ; free xmm%u", hwReg.indexInClass());
    }
    if (isTempVecD(hwReg))
      vecTemp_.free(hwReg.indexInClass());
  }
}
void Emitter::syncAndFreeTempReg(HWReg hwReg) {
  if (!hwReg.isValid() || !isTemp(hwReg) ||
      !hwRegs_[hwReg.combinedIndex()].contains.isValid()) {
    return;
  }
  _spillTempForFR(hwReg);
  freeReg(hwReg);
}

// TODO: check wherger we should make this call require a temp reg.
HWReg Emitter::useReg(HWReg hwReg) {
  if (!hwReg.isValid())
    return hwReg;
  // Check whether it is a temporary.
  if (hwReg.isGpX()) {
    if (isTempGpX(hwReg))
      gpTemp_.use(hwReg.indexInClass());
  } else {
    if (isTempVecD(hwReg))
      vecTemp_.use(hwReg.indexInClass());
  }
  return hwReg;
}

void Emitter::_spillTempForFR(HWReg toSpill) {
  assert(isTemp(toSpill));

  HWRegState &hwState = hwRegs_[toSpill.combinedIndex()];
  FR fr = hwState.contains;
  hwState.contains = {};
  assert(fr.isValid() && "Allocated tmp register is unused");

  FRState &frState = frameRegs_[fr.index()];

  assert(frState.globalReg != toSpill && "global regs can't be temporary");
  if (frState.globalReg) {
    if (!frState.globalRegUpToDate) {
      movHWFromHW<false>(frState.globalReg, toSpill);
      frState.globalRegUpToDate = true;
    }
  } else {
    if (!frState.frameUpToDate) {
      _storeHWToFrame(fr, toSpill);
      frState.frameUpToDate = true;
    }
  }

  if (frState.localGpX == toSpill)
    frState.localGpX = {};
  else if (frState.localVecD == toSpill)
    frState.localVecD = {};
  else
    assert(false && "local reg not used by FR");
}

void Emitter::syncToFrame(FR fr) {
  FRState &frState = frameRegs_[fr.index()];
  if (frState.frameUpToDate)
    return;

  HWReg hwReg = _isFRInRegister(fr);
  assert(
      hwReg.isValid() && "FR is not synced to frame and is not in a register");

  // We have an invariant that the global reg cannot have an old value if the
  // frame has a new one.
  if (frState.globalReg && !frState.globalRegUpToDate) {
    assert(hwReg != frState.globalReg && "FR is in a global reg");
    movHWFromHW<false>(frState.globalReg, hwReg);
    frState.globalRegUpToDate = true;
  }
  _storeHWToFrame(fr, hwReg);
}

void Emitter::syncAllFRTempExcept(FR exceptFR) {
  // Inside a try region, excluding the destination is unsound: if the
  // instruction throws instead of writing its destination, the catch
  // handler reads the destination's frame slot, and register
  // allocation may have coalesced a live variable's phi with that
  // destination (see test/jit/try-catch-dest-reg.js). Sync everything
  // so the frame is correct along the exceptional edge. Costs one
  // extra store per throwing instruction, only in functions with try.
  if (exceptFR.isValid() && isInTry())
    exceptFR = FR();

  for (unsigned i = 0, e = frameRegs_.size(); i < e; ++i) {
    auto &state = frameRegs_[i];
    FR fr{i};
    if (fr == exceptFR)
      continue;

    // If there is a global reg, just sync to that if needed.
    if (state.globalReg) {
      if (state.globalRegUpToDate)
        continue;
      // Note that it is valid to have no local reg even if the global reg is
      // not up to date, because the FR may be uninitialized.
      if (state.localGpX) {
        comment("    ; sync: r%u (r%u)", state.localGpX.indexInClass(), i);
        movHWFromHW<false>(state.globalReg, HWReg{state.localGpX});
        state.globalRegUpToDate = true;
      } else if (state.localVecD) {
        comment("    ; sync: xmm%u (r%u)", state.localVecD.indexInClass(), i);
        movHWFromHW<false>(state.globalReg, HWReg{state.localVecD});
        state.globalRegUpToDate = true;
      }
      continue;
    }

    // There is no global reg, we must sync to the frame. If the frame is
    // already up to date, we can skip this FR.
    if (state.frameUpToDate)
      continue;

    // Note that it is valid to have no local reg even if the frame is not up to
    // date, because the FR may be uninitialized.
    // x86-64: arm64 pairs two adjacent stores into an stp here. x86 has no
    // store-pair instruction, so each FR is stored on its own.
    if (state.localGpX) {
      comment("    ; sync: r%u (r%u)", state.localGpX.indexInClass(), i);
      _storeHWToFrame(fr, state.localGpX);
    } else if (state.localVecD) {
      comment("    ; sync: xmm%u (r%u)", state.localVecD.indexInClass(), i);
      _storeHWToFrame(fr, state.localVecD);
    }
  }
}

void Emitter::freeAllFRTempExcept(FR exceptFR) {
  // x86-64: the GP temps are two disjoint ranges (the callee-saved and
  // stack registers sit between them), while the vector temps are one.
  for (unsigned i = kGPTemp1.first; i <= kGPTemp2.second; ++i) {
    if (i > kGPTemp1.second && i < kGPTemp2.first)
      continue;

    HWReg hwReg(i, HWReg::GpX{});
    FR fr = hwRegs_[hwReg.combinedIndex()].contains;
    if (!fr.isValid() || fr == exceptFR)
      continue;
    freeFRTemp(fr);
  }

  for (unsigned i = kVecTemp.first; i <= kVecTemp.second; ++i) {
    HWReg hwReg(i, HWReg::VecD{});
    FR fr = hwRegs_[hwReg.combinedIndex()].contains;
    if (!fr.isValid() || fr == exceptFR)
      continue;
    freeFRTemp(fr);
  }
}

void Emitter::freeFRTemp(FR fr) {
  auto &frState = frameRegs_[fr.index()];
  if (frState.localGpX) {
    assert(isTempGpX(frState.localGpX));
    comment(
        "    ; free r%u (r%u)", frState.localGpX.indexInClass(), fr.index());
    hwRegs_[frState.localGpX.combinedIndex()].contains = {};
    gpTemp_.free(frState.localGpX.indexInClass());
    frState.localGpX = {};
  }
  if (frState.localVecD) {
    assert(isTempVecD(frState.localVecD));
    comment(
        "    ; free xmm%u (r%u)", frState.localVecD.indexInClass(), fr.index());
    hwRegs_[frState.localVecD.combinedIndex()].contains = {};
    vecTemp_.free(frState.localVecD.indexInClass());
    frState.localVecD = {};
  }
}

void Emitter::_assignAllocatedLocalHWReg(FR fr, HWReg hwReg) {
  hwRegs_[hwReg.combinedIndex()].contains = fr;
  if (hwReg.isGpX()) {
    comment("    ; alloc: r%u <- r%u", hwReg.indexInClass(), fr.index());
    frameRegs_[fr.index()].localGpX = hwReg;
  } else {
    comment("    ; alloc: xmm%u <- r%u", hwReg.indexInClass(), fr.index());
    frameRegs_[fr.index()].localVecD = hwReg;
  }
}

HWReg Emitter::_isFRInRegister(FR fr) {
  auto &frState = frameRegs_[fr.index()];
  if (frState.localGpX)
    return useReg(frState.localGpX);
  if (frState.localVecD)
    return useReg(frState.localVecD);
  if (frState.globalReg)
    return frState.globalReg;
  return {};
}

HWReg Emitter::getOrAllocFRInVecD(
    FR fr,
    bool load,
    llvh::Optional<HWReg> preferred) {
  auto &frState = frameRegs_[fr.index()];

  assert(!(load && frState.regIsDirty) && "Local is dirty");
#ifndef NDEBUG
  if (!load)
    frState.regIsDirty = true;
#endif

  if (frState.localVecD) {
    return useReg(frState.localVecD);
  }

  // Do we have a global VecD allocated to this FR?
  if (frState.globalReg.isValidVecD()) {
    // If the caller requires that the latest value is present, but it isn't,
    // we need to put it there.
    if (load && !frState.globalRegUpToDate) {
      assert(
          frState.localGpX &&
          "If globalReg is not up to date, there must be a localReg");
      movHWFromHW<true>(frState.globalReg, frState.localGpX);
      frState.globalRegUpToDate = true;
    }

    return frState.globalReg;
  }

  // We have neither global nor local VecD, so we must allocate a new tmp reg.
  HWReg hwVecD = allocTempVecD(preferred);
  _assignAllocatedLocalHWReg(fr, hwVecD);

  if (load) {
    if (frState.localGpX) {
      movHWFromHW<false>(hwVecD, frState.localGpX);
    } else if (frState.globalReg.isValidGpX()) {
      assert(
          frState.globalRegUpToDate &&
          "globalReg must be up to date if no local regs");
      movHWFromHW<false>(hwVecD, frState.globalReg);
    } else {
      _loadFrame(hwVecD, fr);
      assert(frState.frameUpToDate && "frame not up-to-date");
    }
  }

  return hwVecD;
}

HWReg Emitter::getOrAllocFRInGpX(
    FR fr,
    bool load,
    llvh::Optional<HWReg> preferred) {
  auto &frState = frameRegs_[fr.index()];

  assert(!(load && frState.regIsDirty) && "Local is dirty");
#ifndef NDEBUG
  if (!load)
    frState.regIsDirty = true;
#endif

  if (frState.localGpX) {
    assert(!(load && frState.regIsDirty) && "Local is dirty");
    return useReg(frState.localGpX);
  }

  // Do we have a global GpX allocated to this FR?
  if (frState.globalReg.isValidGpX()) {
    // If the caller requires that the latest value is present, but it isn't,
    // we need to put it there.
    if (load && !frState.globalRegUpToDate) {
      assert(
          frState.localVecD &&
          "If globalReg is not up to date, there must be a localReg");
      movHWFromHW<true>(frState.globalReg, frState.localVecD);
      frState.globalRegUpToDate = true;
    }

    return frState.globalReg;
  }

  // We have neither global nor local GpX, so we must allocate a new tmp reg.
  HWReg hwGpX = allocTempGpX(preferred);
  _assignAllocatedLocalHWReg(fr, hwGpX);

  if (load) {
    if (frState.localVecD) {
      movHWFromHW<false>(hwGpX, frState.localVecD);
    } else if (frState.globalReg.isValidVecD()) {
      assert(
          frState.globalRegUpToDate &&
          "globalReg must be up to date if no local regs");
      movHWFromHW<false>(hwGpX, frState.globalReg);
    } else {
      assert(frState.frameUpToDate && "frame not up-to-date");
      _loadFrame(hwGpX, fr);
    }
  }

  return hwGpX;
}

HWReg Emitter::getOrAllocFRInAnyReg(
    FR fr,
    bool load,
    llvh::Optional<HWReg> preferred) {
  if (HWReg tmp = _isFRInRegister(fr))
    return tmp;

  // We have neither global nor local reg, so we must allocate a new tmp reg.
  HWReg hwReg{};
  if (preferred && preferred->isVecD()) {
    hwReg = allocTempVecD(preferred);
  } else {
    hwReg = allocTempGpX(preferred);
  }
  _assignAllocatedLocalHWReg(fr, hwReg);

  if (load) {
    assert(
        frameRegs_[fr.index()].frameUpToDate &&
        "Frame must be up to date when loading");
    _loadFrame(hwReg, fr);
  }

  return hwReg;
}

void Emitter::frUpdatedWithHW(FR fr, HWReg hwReg, FRType localType) {
  FRState &frState = frameRegs_[fr.index()];

  if (LLVM_UNLIKELY(emitTypeAsserts_))
    recordFRWriteForAssert(fr);

  frState.frameUpToDate = false;
#ifndef NDEBUG
  frState.regIsDirty = false;
#endif

  if (frState.globalReg == hwReg) {
    frState.globalRegUpToDate = true;

    if (frState.localGpX)
      freeReg(frState.localGpX);
    if (frState.localVecD)
      freeReg(frState.localVecD);
  } else {
    frState.globalRegUpToDate = false;
    if (hwReg == frState.localGpX) {
      freeReg(frState.localVecD);
    } else {
      assert(
          hwReg == frState.localVecD &&
          "Updated reg doesn't match any FRState register");
      freeReg(frState.localGpX);
    }
  }
  frUpdateType(fr, localType);
}

void Emitter::frUpdateType(FR fr, FRType type) {
  frameRegs_[fr.index()].localType = type;
}

void Emitter::recordFRWriteForAssert(FR fr) {
  FRState &frState = frameRegs_[fr.index()];
  // The two conditions coincide today only because enter()'s allocation
  // loops set globalReg and globalType together and stop together. Pin it,
  // because the check below relies on it.
  assert(
      (frState.globalType != FRType::UnknownPtr) ==
          frState.globalReg.isValid() &&
      "globalType and globalReg must agree");
  if (frState.globalType == FRType::UnknownPtr)
    return;
  if (llvh::is_contained(typeAssertPendingWrites_, fr))
    return;
  typeAssertPendingWrites_.push_back(fr);
}

} // namespace hermes::vm::x86_64

#endif // HERMESVM_JIT_X86_64

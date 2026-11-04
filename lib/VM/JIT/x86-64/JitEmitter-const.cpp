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

void Emitter::loadConstDouble(FR frRes, double val, const char *name) {
  comment("// LoadConst%s r%u, %f", name, frRes.index(), val);
  HWReg hwRes{};

  // Check bitwise for zero because it may be -0.
  if (llvh::DoubleToBits(val) == llvh::DoubleToBits(0)) {
    // TODO: this check should be wider.
    hwRes = getOrAllocFRInVecD(frRes, false);
    a.vpxor(hwRes.xmm(), hwRes.xmm(), hwRes.xmm());
  } else {
    // x86-64: every other double is one `mov gpq, imm64`, so arm64's
    // fp-immediate and constant-pool cases both collapse into the Gp case.
    // TODO: a double that is only ever consumed by arithmetic would be
    // better materialized in a vector register, at the cost of a constant
    // pool entry. Revisit when the arithmetic emitters land.
    hwRes = getOrAllocFRInGpX(frRes, false);
    a.mov(hwRes.gpq(), asmjit::Imm(llvh::DoubleToBits(val)));
  }
  frUpdatedWithHW(frRes, hwRes, FRType::Number);
}

void Emitter::loadSmallHermesValueInGpX(
    const x86::Gp &dest,
    SmallHermesValue shv,
    const char *constName) {
  if constexpr (sizeof(SmallHermesValue) == 4) {
    a.mov(dest.r32(), asmjit::Imm(shv.getRaw()));
  } else {
    loadBits64InGp(dest, shv.getRaw(), constName);
  }
}

void Emitter::loadConstStringInGpX(SymbolID id, const x86::Gp &out) {
  static_assert(
      std::is_same_v<
          RuntimeOffsets::IdentifierTableLookupVectorType,
          TransparentConservativeVector<
              RuntimeOffsets::IdentifierTableLookupEntryType>>,
      "lookupVector_ must be transparent");
  // out = identifierTable_.lookupVector_.ptr
  a.mov(
      out,
      x86::qword_ptr(
          xRuntime,
          (int32_t)(RuntimeOffsets::identifierTable +
                    RuntimeOffsets::identifierTableLookupVector +
                    TransparentConservativeVector<
                        RuntimeOffsets::IdentifierTableLookupEntryType>::
                        dataPointerOffset())));
  // out = out[symID.index].strPrim_
  size_t offset = ((size_t)id.unsafeGetIndex() *
                   RuntimeOffsets::identifierTableLookupEntrySize) +
      RuntimeOffsets::identifierTableLookupEntryStrPrim;
  // x86-64: arm64 needs a register-offset fallback here; a disp32 spans the
  // whole lookup vector in practice, so the load is unconditional -- but
  // only in practice. SymbolID's index is 28 bits (SymbolID::NUM_BITS == 29,
  // minus the not-uniqued tag bit), and LookupEntry is 16 bytes, so the
  // theoretical maximum offset is (2^28 - 1) * 16 ~= 4.29GB, which exceeds
  // INT32_MAX (~2.147GB) well before it exceeds the lookup vector's own
  // representable size. Decline rather than silently truncate the disp32.
  if (LLVM_UNLIKELY(offset > (size_t)INT32_MAX))
    unsupported("const string offset");
  a.mov(out, x86::qword_ptr(out, (int32_t)offset));
}

void Emitter::loadConstBits64(
    FR frRes,
    uint64_t bits,
    FRType type,
    const char *name) {
  comment(
      "// LoadConst%s r%u, %llu",
      name,
      frRes.index(),
      (unsigned long long)bits);
  HWReg hwRes = getOrAllocFRInGpX(frRes, false);

  loadBits64InGp(hwRes.gpq(), bits, name);
  frUpdatedWithHW(frRes, hwRes, type);
}

} // namespace hermes::vm::x86_64

#endif // HERMESVM_JIT_X86_64

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

#include "llvh/Support/Compiler.h"

namespace hermes::vm::x86_64 {

void Emitter::bumpAllocAndUnpoison(
    uint32_t sz,
    const x86::Gp &out,
    const x86::Gp &temp1,
    const x86::Gp &temp2,
    const asmjit::Label &slowPathLab) {
#if HERMESVM_GCKIND == _HERMESVM_GCVALUE_HADES
  // Load the current YG level and end address.
  a.mov(out, x86::qword_ptr(xRuntime, RuntimeOffsets::runtimeHadesYGLevel));
  a.mov(temp1, x86::qword_ptr(xRuntime, RuntimeOffsets::runtimeHadesYGEnd));

  // Try to increment the heap level by the size of the object.
  // x86-64: arm64 splits on whether the size encodes as an add/sub immediate;
  // here the split is whether it fits lea's signed 32-bit displacement. No
  // cell this backend allocates comes close to that, but the encoding depends
  // on the size, so the fallback is kept rather than asserted away.
  if (LLVM_LIKELY(sz <= (uint32_t)INT32_MAX)) {
    a.lea(temp2, x86::ptr(out, (int32_t)sz));
  } else {
    a.mov(temp2, asmjit::Imm(sz));
    a.add(temp2, out);
  }
  a.cmp(temp2, temp1);
  a.ja(slowPathLab);

#if LLVM_ADDRESS_SANITIZER_BUILD
  // Save all the temporary registers.
  //
  // x86-64: this relies on the precondition documented on the declaration
  // in JitEmitter.h -- no live value in any vector temp or xScratch across
  // this call -- so only the 8 GP temps (kGPTemp1/kGPTemp2) need saving.
  // Every one of them is caller-saved, so all of them are pushed; the
  // callers' result register may instead be a callee-saved global, which
  // the callee preserves for us. arm64 has the same shape: its save set
  // likewise excludes its scratch register and vector temps.
  //
  // The count must be even to keep rsp 16-byte aligned across the call, which
  // is the same property arm64's paired stp relies on. Rather than assert
  // that separately, the pushes are counted into rspDelta_ (as putByIdImpl's
  // are), so callRuntime()'s "multiple of 16" assert checks the alignment of
  // this call site too, and the instruction-boundary assert checks that the
  // pops below restore rsp exactly. The counter therefore describes ALL
  // transient rsp movement in the backend, with no scope split.
  constexpr unsigned kNumGPTemps = (kGPTemp1.second - kGPTemp1.first + 1) +
      (kGPTemp2.second - kGPTemp2.first + 1);
  static_assert(kNumGPTemps % 2 == 0, "pushes must preserve rsp alignment");
  for (unsigned i = kGPTemp1.first; i <= kGPTemp1.second; ++i)
    a.push(x86::gpq(i));
  for (unsigned i = kGPTemp2.first; i <= kGPTemp2.second; ++i)
    a.push(x86::gpq(i));
#ifndef NDEBUG
  rspDelta_ += 8 * kNumGPTemps;
#endif

  // rdi is written before rsi, so an `out` that happens to live in either
  // argument register is still read before it is overwritten.
  a.mov(x86::rdi, out);
  a.mov(x86::rsi, asmjit::Imm(sz));
  // Unpoison the newly allocated memory.
  EMIT_RUNTIME_CALL_WITHOUT_SAVED_IP(
      *this,
      void (*)(void const volatile *, size_t),
      __asan_unpoison_memory_region);

  // Restore the temporary registers.
  for (int i = kGPTemp2.second; i >= (int)kGPTemp2.first; --i)
    a.pop(x86::gpq(i));
  for (int i = kGPTemp1.second; i >= (int)kGPTemp1.first; --i)
    a.pop(x86::gpq(i));
#ifndef NDEBUG
  rspDelta_ -= 8 * kNumGPTemps;
#endif
#endif

  // Allocating succeeded, update the level.
  a.mov(x86::qword_ptr(xRuntime, RuntimeOffsets::runtimeHadesYGLevel), temp2);
#else
  // MallocGC does not support inline allocation.
  a.jmp(slowPathLab);
#endif
}

void Emitter::initGCCell(
    CellKind kind,
    uint32_t sz,
    const x86::Gp &cell,
    const x86::Gp &temp1) {
  // Initialize the fields on GCCell.
#ifndef NDEBUG
  // cell->magic = GCCell::kMagic
  // x86-64: a 16-bit store of an immediate needs no register, so arm64's
  // mov-then-strh is a single instruction here and temp1 stays untouched.
  static_assert(sizeof(SHGCCell::magic) == 2);
  a.mov(
      x86::word_ptr(cell, offsetof(SHGCCell, magic)),
      asmjit::Imm(RuntimeOffsets::gcCellMagicValue));

  // cell->debugAllocationId = heap->debugAllocationCounter_++;
  a.mov(
      temp1,
      x86::qword_ptr(xRuntime, RuntimeOffsets::runtimeDebugAllocCounter));
  static_assert(sizeof(SHGCCell::debugAllocationId) == 4);
  a.mov(
      x86::dword_ptr(cell, offsetof(SHGCCell, debugAllocationId)), temp1.r32());
  a.add(temp1, asmjit::Imm(1));
  a.mov(
      x86::qword_ptr(xRuntime, RuntimeOffsets::runtimeDebugAllocCounter),
      temp1);
#endif

  // Load the KindAndSize into a register.
  KindAndSize ks{kind, sz};
  CompressedPointer::RawType rawKS;
  static_assert(sizeof(ks) == sizeof(rawKS));
  memcpy(&rawKS, &ks, sizeof(ks));
  loadBits64InGp(temp1, (uint64_t)rawKS, "KindAndSize");

  // KindAndSize has the same size as a compressed pointer, so store it as one.
  emit_store_cp(a, temp1, x86::ptr(cell, offsetof(SHGCCell, kindAndSize)));
}

void Emitter::allocInYoung(
    CellKind kind,
    uint32_t sz,
    const x86::Gp &out,
    const x86::Gp &temp1,
    const x86::Gp &temp2,
    const asmjit::Label &slowPathLab) {
  // Ensure the size is aligned as required.
  sz = heapAlignSize(sz);
  bumpAllocAndUnpoison(sz, out, temp1, temp2, slowPathLab);
  initGCCell(kind, sz, out, temp1);
}

void Emitter::alloc2InYoung(
    CellKind kind1,
    uint32_t sz1,
    CellKind kind2,
    uint32_t sz2,
    const x86::Gp &out1,
    const x86::Gp &out2,
    const x86::Gp &temp,
    const asmjit::Label &slowPathLab) {
  // Ensure the size is aligned as required.
  sz1 = heapAlignSize(sz1);
  sz2 = heapAlignSize(sz2);
  bumpAllocAndUnpoison(
      sz1 + sz2, out1, /* temp1 */ out2, /* temp2 */ temp, slowPathLab);

  // Initialize first cell.
  initGCCell(kind1, sz1, out1, temp);

  // Place the pointer to the second cell in out2 and initialize the GCCell.
  // x86-64: see bumpAllocAndUnpoison() for why the size is a lea displacement.
  if (LLVM_LIKELY(sz1 <= (uint32_t)INT32_MAX)) {
    a.lea(out2, x86::ptr(out1, (int32_t)sz1));
  } else {
    a.mov(out2, asmjit::Imm(sz1));
    a.add(out2, out1);
  }
  initGCCell(kind2, sz2, out2, temp);
}

} // namespace hermes::vm::x86_64

#endif // HERMESVM_JIT_X86_64

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

// Regression test for the try-region destination-sync bug (dz
// 01a03bbd-02cf), fixed in Emitter::syncAllFRTempExcept() in both
// backends. The test compares interpreter and JIT output; before the fix
// the JIT printed `caught y=0` where the interpreter -- and the spec --
// print `caught y=prior`.
//
// `y = o.bad` never completes: the getter throws, so `y` must still hold
// "prior" when the handler reads it.
//
// MECHANISM. Every emitter that can throw calls syncAllFRTempExcept()
// before the call, and passes its own destination FR as the exception, on
// the assumption that the destination is dead across the call. Inside a
// try region that assumption is wrong: register allocation coalesces `y`'s
// phi with the GetById destination, so the excluded FR is also the FR
// holding the value live into the catch handler. This file's own
// -Xdump-jitcode=3 output showed the prior value being dropped rather than
// stored -- the synced FR got a store instruction and the excluded one did
// not:
//
//     // LoadConstString r0, stringID 5   <- y = "prior", in a temp only
//         ; alloc: r0 <- r0
//     // LoadParam r1, 1
//         ; alloc: r1 <- r1
//     // getById r0, r1, cache 0, symID 944
//         ; sync: r1 (r1)                 <- operand FR1: synced...
//         mov qword ptr [r14+16], rcx     <- ...with a real store
//         ; free r0 (r0)                  <- dest FR0: dropped, no store
//         ; alloc: r0 <- r0               <- rax re-taken for the result
//
// After the longjmp the handler read FR0 out of the memory frame, which
// still held the zero fill _sh_enter wrote on entry -- hence 0. With the
// fix, syncAllFRTempExcept() ignores the exclusion when isInTry(), so FR0
// is stored too and the handler sees "prior":
//
//     // getById r0, r1, cache 0, symID 944
//         ; sync: r0 (r0)                 <- dest FR0: now synced...
//         mov qword ptr [r14+8], rax      <- ...with a real store
//         ; sync: r1 (r1)
//         mov qword ptr [r14+16], rcx
//
// SCOPE. The defect reached 54 destination-excluding sites per backend,
// 108 across both, in JitEmitter-*.cpp: 39 of the guarded ternary form
// (`frRes != x ? frRes : FR()`) and 15 UNCONDITIONAL
// `syncAllFRTempExcept(frRes)`. The unconditional ones were the more
// exposed of the two, having no aliasing guard at all. The fix is central,
// in syncAllFRTempExcept() itself, so all of them are covered at once.
// It reproduced on HV64, HV32 and BOXED alike, and only at -O:
// at -O0 the value reaches the frame anyway and the output was correct.
// Plain calls were NOT affected -- CallInst syncs the whole frame, so
// `y = thrower()` was fine; it took a throwing helper call whose
// destination is excluded, such as a property read of a throwing getter.
// -Xjit-emit-asserts and -Xjit-emit-type-asserts could not catch it: the
// FR holds a well-formed HermesValue, just a stale one, so this was a
// lost-store bug and not a type-tag bug.

var thrower = {
  get bad() {
    throw new Error("boom");
  },
};

function destLive(o) {
  var y = "prior";
  try {
    y = o.bad;
  } catch (e) {
    return "caught y=" + y;
  }
  return "ok y=" + y;
}

print(destLive(thrower));
// CHECK: caught y=prior

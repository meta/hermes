/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error %s | %FileCheck --match-full-lines %s
// REQUIRES: jit
// XFAIL: jit-arch-arm64, jit-arch-x86-64

// KNOWN BUG, DELIBERATELY LEFT FAILING. Both backends are affected; the
// fix is maintainer-bound (see doc/JIT.md, "destination-FR exclusion from
// pre-call syncs in try regions"), so this file records the defect instead
// of hiding it. When the fix lands on both backends, this test XPASSes and
// lit flags it -- drop the XFAIL line then. If a fix lands on only ONE
// backend, remove that backend's feature from the XFAIL list above (and
// leave the other backend's feature in place) so the fixed arch starts
// passing for real while the still-broken one stays honestly XFAIL.
//
// Expected (what the interpreter prints, and what the spec requires):
//     caught y=prior
// Actual, under -Xjit=force at -O on both arm64 and x86-64:
//     caught y=0
//
// `y = o.bad` never completes -- the getter throws -- so `y` must still
// hold "prior" when the handler reads it. It does not.
//
// MECHANISM. Every emitter that can throw calls syncAllFRTempExcept()
// before the call, and passes its own destination FR as the exception, on
// the assumption that the destination is dead across the call. Inside a
// try region that assumption is wrong: register allocation coalesces `y`'s
// phi with the GetById destination, so the excluded FR is also the FR
// holding the value live into the catch handler. This file's own
// -Xdump-jitcode=3 output shows the prior value being dropped rather than
// stored -- note that the synced FR gets a store instruction and the
// excluded one does not:
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
// After the longjmp the handler reads FR0 out of the memory frame, which
// still holds the zero fill _sh_enter wrote on entry -- hence 0.
//
// SCOPE. 54 destination-excluding sites per backend, 108 across both, in
// JitEmitter-*.cpp: 39 of the guarded ternary form
// (`frRes != x ? frRes : FR()`) and 15 UNCONDITIONAL
// `syncAllFRTempExcept(frRes)`. The unconditional ones are the more
// exposed of the two, having no aliasing guard at all.
// Reproduces on HV64, HV32 and BOXED alike. Only at -O:
// at -O0 the value reaches the frame anyway and the output is correct.
// Plain calls are NOT affected -- CallInst syncs the whole frame, so
// `y = thrower()` is fine; it takes a throwing helper call whose
// destination is excluded, such as a property read of a throwing getter.
// -Xjit-emit-asserts and -Xjit-emit-type-asserts cannot catch it: the FR
// holds a well-formed HermesValue, just a stale one, so this is a
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

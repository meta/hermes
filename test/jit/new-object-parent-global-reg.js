/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -typed -Xjit=force -Xjit-crash-on-error %s | %FileCheck --match-full-lines %s
// REQUIRES: jit

// Regression test for NewObjectWithParent clobbering its parent operand.
//
// NewObjectWithParent used to compute the new object's
// (possibly compressed) parent pointer *in place*, overwriting the register
// holding the parent operand, on the assumption that the operand was dead.
//
// Here the parent is the `null` literal, which the register allocator pins to
// a global (callee-saved) register that stays live well past the object
// creation -- it is reused below as the right-hand side of `el == null`.
// Clobbering that register left the comparison reading garbage (zero) instead
// of `null`, so `null == null` produced `false`.
//
// REDUCED COVERAGE ON x86-64. The description above is the arm64 allocation.
// x86-64 has three global GP registers (rbx, r12, r13) against arm64's
// sixteen, and here they go to FR0/FR1/FR2 while the `null` literal is FR5.
// FR5 therefore lands in a temp GP register that the emitter syncs to the
// memory frame before NewObjectWithParent runs, so the "parent operand is
// live in a callee-saved register across the object creation" shape this
// test was written for is NOT reproduced on x86-64; the test still checks
// the observable result, but on that backend it is a plain behavioral test.
// Verified by reading -Xdump-jitcode=3 for this file: the prologue prints
// `alloc: r3 <= r0` / `r12 <= r1` / `r13 <= r2`, and NewObjectWithParent is
// preceded by `sync: r0 (r5)`. Constructing an x86-64 equivalent would mean
// pinning a long-lived `null` into one of only three global registers in a
// function that also uses NewObjectWithParent, which the allocator's
// priority order does not let a test choose; left uncovered rather than
// faked.

(function () {
  var mixed: (number | void | null)[] = [1, null];
  for (var i: number = 0; i < mixed.length; ++i) {
    var el = mixed[i];
    print(i, el == null);
  }
})();
// CHECK: 0 false
// CHECK-NEXT: 1 true

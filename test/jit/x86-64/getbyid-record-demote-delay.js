/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xjit-max-recompiles=0 -Xjit-emit-counters %s 2>&1 | %FileCheck %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// The ById twin of test/jit/recompile-getval-demote-delay.js.
// Two sites, identical decline volume (256 each, so 4 crossings each:
// NumRecompileChecks 8), differing only in whether a knowledge
// transition lands inside the stability window. With a budget of 0 no
// shape is ever actionable, so only the `changed` branch can reset
// stability.
//
// getA: 256 missing-property reads: changed at crossing 1, stable at
// 2, 3, 4 -> demoted at the 256th decline.
// getB: the same, except decline 129 (first of the third window) reads
// an existing own property: a new shape, `changed`, so crossing 3
// resets and crossing 4 reaches only 1. Exactly ONE demotion; deleting
// the ById `changed` reset makes this read 2.
function main() {
  function getA(o) {
    return o.p;
  }
  function getB(o) {
    return o.p;
  }
  function drive(f, o, n) {
    for (var i = 0; i < n; ++i)
      f(o);
  }
  drive(getA, {}, 256);
  drive(getB, {}, 128);
  getB({p: 1});
  drive(getB, {}, 127);
  print('done');
}
main();

// CHECK: done
// CHECK: JIT counters:
// CHECK: NumRecompileChecks: 8{{$}}
// CHECK: NumByIdDemotions: 1{{$}}

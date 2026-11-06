/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xjit-max-recompiles=0 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=BUDGET0 %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=DEFAULT %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// Demotion when no recompile can ever happen (spec: "The demotion
// rule": "When no recompile can ever happen ... every observed site is
// demotion-eligible"). store()'s ByVal site sees only Int32Array
// targets -- learnable evidence, in principle actionable by a
// recompile that adds the typed-array tier -- but with the recompile
// budget forced to 0, `actionablePossible` is false from the first
// crossing on, so `canProgress` can never hold this site's stability
// window open. It demotes on schedule: crossing 1 sees the site's
// first-ever store (a "changed" record), crossings 2-4 are stable, and
// the 4 * 64 = 256th decline flips the slot. NumRecompiles stays 0
// (budget forbids it) and NumRecompileChecks reads 4, one per
// crossing.
//
// At the default budget (2, no override), the very same evidence is
// actionable: the first crossing's progress check succeeds
// immediately (byValShapeProgress), so version 2 installs with the
// Int32Array tier before any site has a chance to go stable. From then
// on every store hits the inline typed-array fast path directly --
// nothing left to decline, nothing left to demote.
function store(a, k, v) {
  a[k] = v;
}
function drive(a, n) {
  for (var i = 0; i < n; ++i)
    store(a, 0, i);
}
var ta = new Int32Array(4);
drive(ta, 300);
print(ta[0]);

// BUDGET0: 299
// BUDGET0: JIT counters:
// BUDGET0: NumRecompileChecks: 4{{$}}
// BUDGET0: NumRecompiles: 0{{$}}
// BUDGET0: NumByValDemotions: 1{{$}}

// DEFAULT: 299
// DEFAULT: JIT counters:
// DEFAULT: NumRecompileChecks: 1{{$}}
// DEFAULT: NumRecompiles: 1{{$}}
// DEFAULT: NumByValDemotions: 0{{$}}

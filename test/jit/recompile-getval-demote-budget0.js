/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xjit-max-recompiles=0 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=BUDGET0 %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=64 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=DEFAULT %s
// RUN: %hermes -fno-inline -dump-bytecode %s | %FileCheck --check-prefix=BC %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// The GetByVal twin of recompile-taval-demote-budget0.js. Demotion when
// no recompile can ever happen (spec: "The demotion rule": "When no
// recompile can ever happen ... every observed site is
// demotion-eligible"). load()'s ByVal site sees only Int32Array targets
// -- learnable evidence, in principle actionable by a recompile that
// adds the typed-array LOAD tier -- but with the recompile budget forced
// to 0, `actionablePossible` is false from the first crossing on, so
// `canProgress` can never hold this site's stability window open. It
// demotes on schedule: crossing 1 sees the site's first-ever read (a
// "changed" record), crossings 2-4 are stable, and the 4 * 64 = 256th
// decline flips the slot. NumRecompiles stays 0 (budget forbids it) and
// NumRecompileChecks reads 4, one per crossing.
//
// At the default budget (2, no override), the very same evidence is
// actionable: the first crossing's progress check succeeds immediately
// (byValShapeProgress), so version 2 installs with the Int32Array LOAD
// tier before any site has a chance to go stable. From then on every
// read hits the inline typed-array fast path directly -- nothing left to
// decline, nothing left to demote.
//
// The key is load's `k` parameter, never a literal: a literal numeric
// key on a READ lowers to GetByIndex (ISel.cpp), which has no tier at
// all -- unlike the put twin's `a[k] = v`, which stays PutByVal even
// with a literal key.
function load(a, k) {
  return a[k];
}
function drive(a, n) {
  for (var i = 0; i < n; ++i)
    load(a, 0);
}
var ta = new Int32Array(4);
ta[0] = 1234;
drive(ta, 300);
// A fresh top-level call: the value read must be the same 1234 whether
// it came back through the demoted plain helper (BUDGET0) or the
// inline typed-array LOAD tier (DEFAULT).
print(load(ta, 0));

// BUDGET0: 1234
// BUDGET0: JIT counters:
// BUDGET0: NumRecompileChecks: 4{{$}}
// BUDGET0: NumRecompiles: 0{{$}}
// BUDGET0: NumByValDemotions: 1{{$}}

// DEFAULT: 1234
// DEFAULT: JIT counters:
// DEFAULT: NumRecompileChecks: 1{{$}}
// DEFAULT: NumRecompiles: 1{{$}}
// DEFAULT: NumByValDemotions: 0{{$}}

// BC-LABEL:Function<load>({{.*}}
// BC: GetByVal

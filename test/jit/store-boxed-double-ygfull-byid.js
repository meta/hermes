/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-emit-counters -Xdump-jitcode=3 -Xhermes-internal-test-methods %s -- direct 2>&1 | %FileCheck --check-prefixes=CHECK,THRESH,CHECK-%hv-mode,COUNT-%hv-mode %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-emit-counters -Xdump-jitcode=3 -Xhermes-internal-test-methods %s -- wide 2>&1 | %FileCheck --check-prefixes=CHECK,THRESH,CHECK-%hv-mode,COUNT-%hv-mode %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xdump-jitcode=3 -Xhermes-internal-test-methods %s -- direct 2>&1 | %FileCheck --check-prefixes=CHECK,THRESH,CHECK-%hv-mode %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xdump-jitcode=3 -Xhermes-internal-test-methods %s -- wide 2>&1 | %FileCheck --check-prefixes=CHECK,THRESH,CHECK-%hv-mode %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-recompile-threshold=4 -Xjit-crash-on-error -Xjit-emit-counters -Xdump-jitcode=3 -Xhermes-internal-test-methods %s -- direct 2>&1 | %FileCheck --check-prefixes=CHECK,FORCE,CHECK-%hv-mode,COUNT-%hv-mode %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-recompile-threshold=4 -Xjit-crash-on-error -Xjit-emit-counters -Xdump-jitcode=3 -Xhermes-internal-test-methods %s -- wide 2>&1 | %FileCheck --check-prefixes=CHECK,FORCE,CHECK-%hv-mode,COUNT-%hv-mode %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-recompile-threshold=4 -Xjit-crash-on-error -Xdump-jitcode=3 -Xhermes-internal-test-methods %s -- direct 2>&1 | %FileCheck --check-prefixes=CHECK,FORCE,CHECK-%hv-mode %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-recompile-threshold=4 -Xjit-crash-on-error -Xdump-jitcode=3 -Xhermes-internal-test-methods %s -- wide 2>&1 | %FileCheck --check-prefixes=CHECK,FORCE,CHECK-%hv-mode %s
// REQUIRES: jit
// UNSUPPORTED: handle_san || gc_malloc

// The PutById counterpart of store-boxed-double-ygfull.js: a box allocation
// in the PutById tier that finds the young generation full declines to
// _jit_put_by_id with nothing stored, and the helper completes the store.
// run() overwrites o.p with a fresh boxing value and reads it back, 524,288
// times (two fills of the largest young generation at the Release box
// size), allocating nothing but boxes. Two objects: p in a direct slot
// ("direct"), and p as the eighth property, in an out-of-line slot inside
// the object's propStorage cell ("wide", the shape store-boxed-double-
// card.js uses). The collection a fallback's helper starts moves the object
// and, for "wide", its propStorage; the read-back after every store checks
// that the helper stored through the moved object.
//
// Startup. The tier is emitted only when the site's write cache names a
// class at compile time, so a cold body holds the helper alone. Each mode
// warms setP with inline integers (two run() calls of 100 stores into a
// throwaway object of the measured class) until the body with the tier is
// installed. Threshold mode: setP has no loop, so its first four calls run
// interpreted and fill the cache, and the fifth compiles it with the tier.
// Force mode: setP's first body is cold (helper only); the helper fills
// the cache, and its fourth decline crosses -Xjit-recompile-threshold=4,
// so version 2 has the tier. run() has a loop, so threshold mode compiles
// it at its second call (4 >> 2 = 1, x86-64/JIT.h); force mode compiles it
// at its first. The CHECK lines on the -Xdump-jitcode=3 output show the
// setP body with the tier, and its box arm under HV32 and BOXED, compiled
// before "warm", and nothing compiled after "warm". Then gc() empties the
// young generation and the measured object is allocated, young.
//
// The first fallback is deterministic: the object is young until the first
// collection, so every store until then takes the young arm, and the box
// allocation is the only allocation in the measured call that can find the
// young generation full. With counters the fallback goes through the
// counting block; without, slowFull is the helper label itself. Counter
// runs: NumPutByIdBoxes and NumStoreBoxFallbacks nonzero under HV32 and
// BOXED, both 0 under HV64; no JIT PutByVal runs, so NumPutByValBoxes is 0.
//
// Fails if the PutById tier's fallback skipped the helper (a PutById
// variant of plan Task 1 mutation E: a mismatch, with and without
// counters), and if it were not counted (mutation F: NumStoreBoxFallbacks
// 0 in the counter runs).

function setP(o, v) {
  o.p = v;
}

function run(setP, o, n, base, step) {
  var bad = 0;
  for (var k = 0; k < n; ++k) {
    var v = base + (2 * k + 1) * step;
    setP(o, v);
    if (o.p !== v)
      ++bad;
  }
  return bad;
}

function main(setP, run, print, gc, wide) {
  var N = 524288;
  var eps = Number.EPSILON;
  function makeObj() {
    return {p: 0};
  }
  function makeWide() {
    return {a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, p: 0};
  }
  var make = wide ? makeWide : makeObj;
  var w = make();
  run(setP, w, 100, 0, 1);
  run(setP, w, 100, 0, 1);
  print('warm');
  gc();
  var o = make();
  print('measure');
  print('mismatches', run(setP, o, N, 1, eps));
}

main(setP, run, print, gc, hermescli.getScriptArgs()[0] === 'wide');

// THRESH: JIT compilation of FunctionID {{[0-9]+}}, 'setP'{{$}}
// FORCE: JIT compilation of FunctionID {{[0-9]+}}, 'setP' (version 2)
// CHECK-NOT: JIT compilation of
// CHECK: // Put to object specialization
// CHECK-NOT: JIT compilation of
// CHECK-HV32: // Box the double in the young generation
// CHECK-BOXED: // Box the double in the young generation
// CHECK-NOT: JIT compilation of
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'setP'
// CHECK: {{^}}warm{{$}}
// CHECK-NOT: JIT compilation of
// CHECK: {{^}}measure{{$}}
// CHECK-NOT: JIT compilation of
// CHECK: {{^}}mismatches 0{{$}}
// COUNT-HV64: NumPutByValBoxes: 0{{$}}
// COUNT-HV64-NEXT: NumPutByIdBoxes: 0{{$}}
// COUNT-HV64-NEXT: NumStoreBoxFallbacks: 0{{$}}
// COUNT-HV32: NumPutByValBoxes: 0{{$}}
// COUNT-HV32-NEXT: NumPutByIdBoxes: {{[1-9][0-9]*$}}
// COUNT-HV32-NEXT: NumStoreBoxFallbacks: {{[1-9][0-9]*$}}
// COUNT-BOXED: NumPutByValBoxes: 0{{$}}
// COUNT-BOXED-NEXT: NumPutByIdBoxes: {{[1-9][0-9]*$}}
// COUNT-BOXED-NEXT: NumStoreBoxFallbacks: {{[1-9][0-9]*$}}

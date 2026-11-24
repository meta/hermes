/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-emit-counters -Xdump-jitcode=2 -Xhermes-internal-test-methods %s -- young 2>&1 | %FileCheck --check-prefixes=CHECK,CHECK-%hv-mode %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-emit-counters -Xdump-jitcode=2 -Xhermes-internal-test-methods %s -- old 2>&1 | %FileCheck --check-prefixes=CHECK,CHECK-%hv-mode %s
// REQUIRES: jit
// UNSUPPORTED: handle_san || gc_malloc

// The PutById tier boxes a double inline, as the PutByVal tier does in
// store-boxed-double-byval.js: NumPutByIdBoxes is N under HV32 and BOXED, 0
// under HV64, with the measured object young and then old.
//
// Threshold mode, because the PutById tier is emitted only when the site's
// write cache names a class at compile time. setP has no loop, so with
// -Xjit-threshold=4 its first four calls run interpreted (filling the cache
// with make()'s class) and the fifth compiles it, tier included
// (x86-64/JIT.h:289-292, Interpreter.cpp:1280). The warm-up makes five calls
// with inline-encodable integers: no box. main has a loop and runs once, so
// it stays interpreted: its own stores and reads count nothing. The CHECK
// lines on the -Xdump-jitcode=2 output show setP compiled before "measure"
// and, with CHECK-NOT, nothing compiled after it, so every measured store
// runs setP's compiled body.
//
// Why exactly N: as in store-boxed-double-byval.js; the measured object comes
// from the same make() as the warm-up one, so it has the cached class, and
// p is a direct slot. Fails before the feature, and with the box arm sent
// to the helper (plan Task 1, mutation A).

function setP(o, v) {
  o.p = v;
}

function main(setP, gc, old) {
  var N = 1000;
  var eps = Number.EPSILON;
  function make() {
    return {p: 0};
  }
  var w = make();
  for (var i = 0; i < 5; ++i)
    setP(w, i);
  gc();
  var o = make();
  if (old)
    gc();
  print('measure');
  var bad = 0;
  for (var i = 0; i < N; ++i) {
    var v = 1 + (2 * i + 1) * eps;
    setP(o, v);
    if (o.p !== v)
      ++bad;
  }
  print('mismatches', bad);
}

main(setP, gc, hermescli.getScriptArgs()[0] === 'old');

// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'setP'
// CHECK: {{^}}measure{{$}}
// CHECK-NOT: JIT compilation of
// CHECK: {{^}}mismatches 0{{$}}
// CHECK: JIT counters:
// CHECK: NumPutByValBoxes: 0{{$}}
// CHECK-HV64-NEXT: NumPutByIdBoxes: 0{{$}}
// CHECK-HV32-NEXT: NumPutByIdBoxes: 1000{{$}}
// CHECK-BOXED-NEXT: NumPutByIdBoxes: 1000{{$}}
// CHECK-NEXT: NumStoreBoxFallbacks: 0{{$}}

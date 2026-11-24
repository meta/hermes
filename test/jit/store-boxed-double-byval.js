/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-emit-counters -Xdump-jitcode=2 -Xhermes-internal-test-methods %s -- young 2>&1 | %FileCheck --check-prefixes=CHECK,CHECK-%hv-mode %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-emit-counters -Xdump-jitcode=2 -Xhermes-internal-test-methods %s -- old 2>&1 | %FileCheck --check-prefixes=CHECK,CHECK-%hv-mode %s
// REQUIRES: jit
// UNSUPPORTED: handle_san || gc_malloc

// The PutByVal JSArray tier stores a double that needs a BoxedDouble without
// leaving JIT code: it allocates the box in the young generation and stores
// the tagged pointer (doc/superpowers/specs/2026-10-29-jit-inline-boxed-
// double-store-design.md). Under HV32 and BOXED each of the N measured
// stores boxes, so NumPutByValBoxes is N; under HV64 nothing boxes and it
// is 0. Run twice: the measured array young ("young"), and promoted to the
// old generation by a second gc() ("old"), where every box store also
// dirties a card.
//
// The values are 1 + (2k + 1) * 2^-52: exact doubles whose lowest mantissa
// bit is set, so their low 35 bits (HV32) and low 3 bits (BOXED) are
// nonzero, and distinct for distinct k.
//
// Why exactly N. -Xjit=force compiles every function at its first call.
// (a) The warm-up call stores the inline-encodable 1 into a throwaway
// array: no box. (b) gc() leaves the young generation empty and, for the one
// or two calls made here, no old-generation collection running and no
// compactee (spec "Testing"; checked on every tree from
// -gc-print-collection-stats, plan Task 1 Step 17). Were either active, an
// old-target store would decline before the box arm and the count would
// fall short: a false failure, never a masked pass.
// (c) The measured array holds N small integers, so every index passes the
// tier's range guard and no element is a hole. (d) The measured loop
// allocates N boxes (24 KB at the Debug box size) and nothing else, far
// below the smallest young generation, so no collection starts inside it
// and every store passes the barrier predicate: the box arm runs N times.
// The CHECK-NOT line on the -Xdump-jitcode=2 output shows nothing is
// compiled from "measure" on. No other JIT PutByVal stores a boxing double
// (the array is filled by push; the read-back is GetByVal), and only the box
// arm counts.
//
// Fails before the feature (0 under HV32 and BOXED: every store declined),
// and with the box arm sent to the helper (plan Task 1, mutation A).

function storeElem(arr, i, v) {
  arr[i] = v;
}

function main(store, gc, old) {
  var N = 1000;
  var eps = Number.EPSILON;
  store([0], 0, 1);
  gc();
  var arr = [];
  for (var i = 0; i < N; ++i)
    arr.push(i);
  if (old)
    gc();
  print('measure');
  for (var i = 0; i < N; ++i)
    store(arr, i, 1 + (2 * i + 1) * eps);
  var bad = 0;
  for (var i = 0; i < N; ++i)
    if (arr[i] !== 1 + (2 * i + 1) * eps)
      ++bad;
  print('mismatches', bad);
}

main(storeElem, gc, hermescli.getScriptArgs()[0] === 'old');

// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeElem'
// CHECK: {{^}}measure{{$}}
// CHECK-NOT: JIT compilation of
// CHECK: {{^}}mismatches 0{{$}}
// CHECK: JIT counters:
// CHECK-HV64: NumPutByValBoxes: 0{{$}}
// CHECK-HV32: NumPutByValBoxes: 1000{{$}}
// CHECK-BOXED: NumPutByValBoxes: 1000{{$}}
// CHECK-NEXT: NumPutByIdBoxes: 0{{$}}
// CHECK-NEXT: NumStoreBoxFallbacks: 0{{$}}

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-emit-counters -Xdump-jitcode=2 -Xhermes-internal-test-methods %s -- byval 2>&1 | %FileCheck --check-prefixes=CHECK,BYVAL-%hv-mode %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-emit-counters -Xdump-jitcode=2 -Xhermes-internal-test-methods %s -- byid 2>&1 | %FileCheck --check-prefixes=CHECK,BYID-%hv-mode %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=4 -Xjit-crash-on-error -Xjit-emit-counters -Xdump-jitcode=2 -Xhermes-internal-test-methods %s -- byidwide 2>&1 | %FileCheck --check-prefixes=CHECK,BYID-%hv-mode %s
// REQUIRES: jit
// UNSUPPORTED: handle_san || gc_malloc

// An old target that receives one inline box gets its card dirtied. The box
// is young by construction, so the store is an old-to-young pointer; if the
// box arm skipped the card, HERMES_SLOW_DEBUG's HadesGC::verifyCardTable()
// (run at the start of every young collection) finds a young pointer in an
// old slot on a clean card, or the read-back after the collection sees a
// freed box. One case per process, so that one case cannot dirty the other's
// card: byval (an element of an old array), byid (a direct slot of an old
// object), byidwide (an out-of-line slot, inside the old object's
// propStorage cell; `p` is the eighth property).
//
// Not masked: the second gc()'s young collection clears every old segment's
// cards (HadesGC.cpp:3100-3110) with no old-generation collection running
// (checked with -gc-print-collection-stats), the case stores exactly one
// value into an old object after it, and every function is compiled before
// "warm" (the CHECK-NOT line on the -Xdump-jitcode=2 output checks that
// nothing compiles after it, so no compilation writes an old object either).
// The only store that can dirty the target's card is the box arm's. churn()
// then allocates more than the largest young generation holds (300,000
// objects of at least 16 bytes is over 4 MiB), forcing a young collection.
// Fails with the box arm's card dirty removed (plan Task 1, mutation B),
// under HERMES_SLOW_DEBUG.

function storeElem(arr, i, v) {
  arr[i] = v;
}

function setP(o, v) {
  o.p = v;
}

function main(storeElem, setP, gc, which) {
  var v = 1 + 3 * Number.EPSILON;
  function makeArr() {
    return [0, 0, 0, 0];
  }
  function makeObj() {
    return {p: 0};
  }
  function makeWide() {
    return {a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, p: 0};
  }
  function alloc(i) {
    return {a: i};
  }
  function churn(n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += alloc(i).a;
    return s;
  }
  var make =
      which === 'byval' ? makeArr : which === 'byid' ? makeObj : makeWide;
  // Compile the store function (five calls), alloc (five calls) and churn
  // (two calls) before anything is measured.
  var w = make();
  for (var i = 0; i < 5; ++i) {
    if (which === 'byval')
      storeElem(w, 0, i);
    else
      setP(w, i);
  }
  churn(5);
  churn(5);
  print('warm');
  gc();
  var t = make();
  gc();
  if (which === 'byval')
    storeElem(t, 0, v);
  else
    setP(t, v);
  var s = churn(300000);
  print('value', (which === 'byval' ? t[0] : t.p) === v, s);
}

main(storeElem, setP, gc, hermescli.getScriptArgs()[0]);

// CHECK: {{^}}warm{{$}}
// CHECK-NOT: JIT compilation of
// CHECK: {{^}}value true 44999850000{{$}}
// CHECK: JIT counters:
// BYVAL-HV64: NumPutByValBoxes: 0{{$}}
// BYVAL-HV64-NEXT: NumPutByIdBoxes: 0{{$}}
// BYVAL-HV32: NumPutByValBoxes: 1{{$}}
// BYVAL-HV32-NEXT: NumPutByIdBoxes: 0{{$}}
// BYVAL-BOXED: NumPutByValBoxes: 1{{$}}
// BYVAL-BOXED-NEXT: NumPutByIdBoxes: 0{{$}}
// BYID-HV64: NumPutByValBoxes: 0{{$}}
// BYID-HV64-NEXT: NumPutByIdBoxes: 0{{$}}
// BYID-HV32: NumPutByValBoxes: 0{{$}}
// BYID-HV32-NEXT: NumPutByIdBoxes: 1{{$}}
// BYID-BOXED: NumPutByValBoxes: 0{{$}}
// BYID-BOXED-NEXT: NumPutByIdBoxes: 1{{$}}
// CHECK-NEXT: NumStoreBoxFallbacks: 0{{$}}

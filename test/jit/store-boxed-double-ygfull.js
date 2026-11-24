/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefixes=CHECK,CHECK-%hv-mode %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error %s | %FileCheck --check-prefix=CHECK %s
// REQUIRES: jit
// UNSUPPORTED: handle_san || gc_malloc

// A box allocation that finds the young generation full declines to the
// helper with nothing stored, and the helper completes the store. The loop
// overwrites slot 0 of a small array with a fresh boxing value and reads it
// back each time; it allocates nothing but boxes, and runs 2 * 4 MiB / 16 =
// 524,288 times: two fills of the largest young generation at the Release
// box size. The first fallback is deterministic: the array is young until
// the first collection, so every store until then takes the young arm, and
// the box allocation is the only allocation that can find the young
// generation full. With counters (first RUN) the fallback goes through the
// counting block; without (second RUN) slowFull is the helper label itself.
// After the first collection the array is old, but this test does not pin
// the box arm's card dirty: the first store after each collection is the
// fallback's helper store, whose runtime barrier dirties the array's card
// before any inline box store lands on it. store-boxed-double-card.js pins
// that card dirty.
//
// Fails if the fallback skipped the helper (mutation E: a mismatch), and if
// it were not counted (mutation F: NumStoreBoxFallbacks 0).

function storeElem(arr, i, v) {
  arr[i] = v;
}

function main(store) {
  var N = 524288;
  var eps = Number.EPSILON;
  var arr = [0, 0];
  var bad = 0;
  for (var k = 0; k < N; ++k) {
    var v = 1 + (2 * k + 1) * eps;
    store(arr, 0, v);
    if (arr[0] !== v)
      ++bad;
  }
  print('mismatches', bad);
}

main(storeElem);

// CHECK: {{^}}mismatches 0{{$}}
// CHECK-HV64: NumPutByValBoxes: 0{{$}}
// CHECK-HV64-NEXT: NumPutByIdBoxes: 0{{$}}
// CHECK-HV64-NEXT: NumStoreBoxFallbacks: 0{{$}}
// CHECK-HV32: NumPutByValBoxes: {{[1-9][0-9]*$}}
// CHECK-HV32-NEXT: NumPutByIdBoxes: 0{{$}}
// CHECK-HV32-NEXT: NumStoreBoxFallbacks: {{[1-9][0-9]*$}}
// CHECK-BOXED: NumPutByValBoxes: {{[1-9][0-9]*$}}
// CHECK-BOXED-NEXT: NumPutByIdBoxes: 0{{$}}
// CHECK-BOXED-NEXT: NumStoreBoxFallbacks: {{[1-9][0-9]*$}}

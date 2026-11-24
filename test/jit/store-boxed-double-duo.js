/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=4 -Xjit-emit-counters -Xdump-jitcode=3 %s 2>&1 | %FileCheck --check-prefixes=CHECK,CHECK-%hv-mode %s
// REQUIRES: jit
// UNSUPPORTED: handle_san || gc_malloc

// A duo PutByVal site boxes inline in its second version, the one that puts
// the typed-array tier ahead of the JSArray tier (the x86-64 tier with the
// highest register pressure). store-boxed-double-values.js checks that
// version 2 emits the box arm; this test checks that the arm runs: K = 1000
// measured stores through version 2's JSArray tier give NumPutByValBoxes K
// and NumStoreBoxFallbacks 0 under HV32 and BOXED, and no box under HV64.
//
// Startup. -Xjit=force compiles storeDuo at its first call, with the JSArray
// tier only. The four warm-up stores into a Float64Array fail its guard and
// call the helper; the fourth decline crosses -Xjit-recompile-threshold=4,
// and the recompile emits version 2, with the typed-array tier first. The
// CHECK lines on the -Xdump-jitcode=3 output show version 2 compiled, with
// both tiers and (HV32, BOXED) the box arm, before "warm", and nothing
// compiled after "warm", so every measured store runs version 2. makeArr is
// warmed too: its push site gets it recompiled, which must happen before
// gc().
//
// Why exactly K.
// (a) Nothing else reaches the JSArray tier's box arm, the only code that
// counts NumPutByValBoxes. The warm-up stores small integers, and version 1
// declines them before any encode. makeArr fills its array with push, a
// call; the read-back is a GetByVal. Every other JIT store in the process
// stores a pointer or a small integer, which never boxes.
// (b) The Float64Array is allocated before gc() and holds 32 bytes. Its
// external-memory charge shrinks the young generation
// (HadesGC::creditExternalMemory) only until gc()'s first young collection
// moves it to the old generation (transferExternalMemoryToOldGen). There it
// adds 32 bytes against the 21 MiB that would start a natural collection
// (spec "Testing", condition 1).
// (c) gc() leaves the young generation empty, with no old-generation
// collection running and no compactee (spec "Testing"). The measured array
// is allocated after it, so it is young: the tier skips the barrier checks.
// It holds K small integers, so every index passes the range guard and no
// element is a hole.
// (d) After gc() the script allocates the measured array (a few KB), the
// printed strings and K boxes (24 KB at the Debug box size), and compiles
// nothing. That is far below the smallest young generation (about 1 MiB),
// so no collection starts and every box allocation succeeds: K boxes, no
// fallback.
//
// Fails with the box arm sent to the helper (plan Task 1, mutation A):
// NumPutByValBoxes 0.

function storeDuo(t, i, v) {
  t[i] = v;
}

function main(store, gc) {
  var K = 1000;
  var eps = Number.EPSILON;
  function makeArr() {
    var a = [];
    for (var i = 0; i < K; ++i)
      a.push(i);
    return a;
  }
  var f64 = new Float64Array(4);
  for (var i = 0; i < 4; ++i)
    store(f64, 1, i);
  makeArr();
  print('warm');
  gc();
  var arr = makeArr();
  print('measure');
  for (var k = 0; k < K; ++k)
    store(arr, k, 1 + (2 * k + 1) * eps);
  var bad = 0;
  for (var k = 0; k < K; ++k)
    if (arr[k] !== 1 + (2 * k + 1) * eps)
      ++bad;
  print('mismatches', bad);
}

main(storeDuo, gc);

// CHECK: JIT compilation of FunctionID {{[0-9]+}}, 'storeDuo' (version 2)
// CHECK-NOT: JIT compilation of
// CHECK: // Inline typed array store (kind {{[0-9]+}})
// CHECK-NOT: JIT compilation of
// CHECK: // Inline fast array store
// CHECK-NOT: JIT compilation of
// CHECK-HV32: // Box the double in the young generation
// CHECK-BOXED: // Box the double in the young generation
// CHECK-NOT: JIT compilation of
// CHECK: JIT successfully compiled FunctionID {{[0-9]+}}, 'storeDuo' (version 2)
// CHECK: {{^}}warm{{$}}
// CHECK-NOT: JIT compilation of
// CHECK: {{^}}measure{{$}}
// CHECK-NOT: JIT compilation of
// CHECK: {{^}}mismatches 0{{$}}
// CHECK: JIT counters:
// CHECK-HV64: NumPutByValBoxes: 0{{$}}
// CHECK-HV32: NumPutByValBoxes: 1000{{$}}
// CHECK-BOXED: NumPutByValBoxes: 1000{{$}}
// CHECK-NEXT: NumPutByIdBoxes: 0{{$}}
// CHECK-NEXT: NumStoreBoxFallbacks: 0{{$}}

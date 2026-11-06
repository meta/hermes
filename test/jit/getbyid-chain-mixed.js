/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// Own, chain and prototype entries at one site. Receivers: ob (p own),
// pr (p on its prototype), cr (p two prototypes up: cr -> mid -> base).
// 1. drive(get, [ob, pr, cr], 24): version 1 declines on calls 1-16,
//    recording an own, a prototype and a chain entry (calls 1-3); call
//    16 recompiles. Version 2 emits own, chain, then the prototype
//    block. Calls 17-24 hit: ob twice (own), pr three times (chain ID
//    miss, then the prototype block), cr three times (chain hits).
//    Sum 8 x 6 = 48.
// 2. unrel.y = 2 bumps the epoch (unrel is flagged by the cached add of
//    kid.k). drive(get, [cr], 2): call 1 misses the chain's epoch
//    guard, misses the prototype block (no entry for cr's class) and
//    the generic tier, and the helper's fast refresh refreshes the
//    entry and returns 3 without a decline; call 2 hits. Sum 6.
// 3. mid.p = 30 shadows base's p at depth 1 (mid is flagged: a bump).
//    drive(get, [cr], 16): every call misses the epoch guard, the
//    prototype block and the generic tier, and declines; the forwarded
//    lookup leaves a prototype hit in the read cache, so call 1 records
//    a prototype entry for cr's class. Call 16 recompiles (the last of
//    the budget of 2). Sum 480.
// 4. Version 3 holds all four entries. drive(get, [ob, pr, cr], 210):
//    every call hits inline. cr passes the chain's ID guard, misses its
//    (stale) epoch guard, and must reach the prototype block with the
//    receiver's class and lazy ID intact to hit there. Sum 70 x 33.
// NumByIdChainHits = 3 + 1 = 4, NumByIdChainRefreshes = 1,
// NumRecompiles = 2 (get), NumByIdDemotions = 0. main declines on its
// global reads (Object, print), the .create reads and three adds, well
// under 16.
// Fails if the chain entry's epoch-miss path clobbers the receiver's
// lazy ID (temp3 on x86-64): cr's calls in phase 4 miss the prototype
// block and decline, and the third resulting threshold crossing
// demotes the site (NumRecompileChecks: 5, NumByIdDemotions: 1). A
// clobbered receiver class (temp2) is instead decoded as a pointer by
// the prototype block, which crashes (on x86-64, a SEGV in emitted
// code); no counter is needed for that.
function main() {
  var ob = {p: 1};
  var pr = Object.create({q: 0, p: 2});
  var base = {w: 0, p: 3};
  var mid = Object.create(base);
  var cr = Object.create(mid);
  var unrel = {};
  var kid = Object.create(unrel);
  kid.k = 1;
  function get(o) {
    return o.p;
  }
  function drive(f, arr, k, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i % k]);
    return s;
  }
  var all = [ob, pr, cr];
  var s1 = drive(get, all, 3, 24);
  unrel.y = 2;
  var s2 = drive(get, [cr], 1, 2);
  mid.p = 30;
  var s3 = drive(get, [cr], 1, 16);
  var s4 = drive(get, all, 3, 210);
  print(s1, s2, s3, s4);
}
main();

// OUT: 48 6 480 2310
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (3 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 1 chain, 0 megamorphic
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 3)
// DUMP: // Get polymorphic chain (4 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 3)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 1 chain, 0 megamorphic
// CNT: NumRecompileChecks: 2{{$}}
// CNT-NEXT: NumRecompiles: 2{{$}}
// CNT: NumByIdDemotions: 0{{$}}
// CNT: NumByIdChainRefreshes: 1{{$}}
// CNT: NumByIdChainHits: 4{{$}}

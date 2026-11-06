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

// One site per holder depth, 100 calls each. A site whose version 1
// records a shape recompiles at its 16th call; its calls 17-100 hit.
// - depth 1: the read cache yields a prototype entry, not a chain entry.
// - depth 2: chain entry; p is 1.1, which does not fit the inline
//   compressed-double encoding (SmallHermesValue.h:323), so on HV32 and
//   BOXED it is a heap BoxedDouble and the load takes emitFirstCase's
//   branch to the shared rest cases (x86 JitEmitter-internal.cpp:24).
//   (2.5 would not: its low 32 bits are zero, so it is stored inline.)
// - depth 3: chain entry; the holder has 7 properties before p, so p's
//   slot (7) is out of line (HERMESVM_DIRECT_PROPERTY_SLOTS is 6).
// - depth 8: chain entry at kMaxChainDepth.
// - depth 9: the walk stops at depth 8 and records nothing; the site
//   never recompiles and demotes at its fourth crossing (call 64).
// NumByIdChainHits = 3 x (100 - 16) = 252. NumRecompiles = 4 (depths 1,
// 2, 3, 8). NumByIdDemotions = 1 (depth 9). mk() declines 13 times in
// total (7 keyed stores, 5 `o.p` adds, one Object.create miss).
// Fails before the feature (0 hits, and depths 2, 3 and 8 demote too),
// and if the depth limit is off by one either way (+84 or -84 hits).
function main() {
  function mk(depth, pad, v) {
    var o = {};
    for (var i = 0; i < pad; ++i)
      o['k' + i] = i;
    o.p = v;
    for (var i = 0; i < depth; ++i)
      o = Object.create(o);
    return o;
  }
  function getD1(o) {
    return o.p;
  }
  function getD2(o) {
    return o.p;
  }
  function getD3(o) {
    return o.p;
  }
  function getD8(o) {
    return o.p;
  }
  function getD9(o) {
    return o.p;
  }
  // Counts the reads that return exactly v, so no floating-point sum is
  // pinned.
  function drive(f, o, n, v) {
    var ok = 0;
    for (var i = 0; i < n; ++i)
      if (f(o) === v)
        ++ok;
    return ok;
  }
  var r1 = mk(1, 0, 1);
  var r2 = mk(2, 0, 1.1);
  var r3 = mk(3, 7, 3);
  var r8 = mk(8, 0, 8);
  var r9 = mk(9, 0, 9);
  print(
      drive(getD1, r1, 100, 1),
      drive(getD2, r2, 100, 1.1),
      drive(getD3, r3, 100, 3),
      drive(getD8, r8, 100, 8),
      drive(getD9, r9, 100, 9));
}
main();

// OUT: 100 100 100 100 100
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getD1' (version 2)
// DUMP: // Get polymorphic chain (1 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'getD1' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 0 chain, 0 megamorphic
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'getD8' (version 2)
// DUMP: // Get polymorphic chain (1 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'getD8' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 1 chain, 0 megamorphic
// DUMP-NOT: 'getD9' (version 2)
// CNT: NumRecompiles: 4{{$}}
// CNT: NumByIdDemotions: 1{{$}}
// CNT: NumByIdChainHits: 252{{$}}

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

// Five receiver classes, p two prototypes up from each, at one site.
// Version 1: calls 1-4 record four chain entries; call 5's shape finds
// no room, so addByIdShape sets megamorphic. Call 16 recompiles and
// version 2 emits the four entries. Of calls 17-1000 (i = 16..999),
// those with i % 5 == 4 are the fifth class (i = 19, 24, ..., 999: 197
// calls); the other 787 hit. The fifth class keeps declining; its
// crossings change nothing (megamorphic is already set), so the third
// demotes the site.
// Fails before the feature, and if the no-room branch skipped
// addByIdShape (the dump would read 0 megamorphic).
function main() {
  function mk(v) {
    return Object.create(Object.create({p: v}));
  }
  function get(o) {
    return o.p;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i % 5]);
    return s;
  }
  print(drive(get, [mk(1), mk(2), mk(3), mk(4), mk(5)], 1000));
}
main();

// OUT: 3000
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (4 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 1 chain, 1 megamorphic
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 1{{$}}
// CNT: NumByIdChainHits: 787{{$}}

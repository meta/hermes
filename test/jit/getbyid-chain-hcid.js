/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-hc-id-limit=1 %s | %FileCheck --check-prefix=OUT %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-hc-id-limit=1 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-hc-id-limit=1 -Xjit-emit-counters %s 2>&1 | %FileCheck --check-prefix=CNT %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// One lazy JIT ID exists in the process. Receivers A and B read p two
// prototypes up, alternating; calls 1 and 2 record A's and B's chain
// entries, in that order, and call 16 recompiles. Version 2 gives A the
// only ID and marks B unemittable, so B is never progress: exactly one
// recompile. B's misses cross three times without change and demote the
// site. A's calls in version 2 (even i in 16..198: 92) hit inline. No
// function compiles warm before get's version 2, so nothing else takes
// the ID.
// Fails if a chain entry with no ID is skipped without `unemittable`
// (B stays progress: a second recompile).
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
      s += f(arr[i & 1]);
    return s;
  }
  print(drive(get, [mk(1), mk(2)], 200));
}
main();

// OUT: 300
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (1 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 1 chain, 0 megamorphic
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 1{{$}}
// CNT: NumByIdChainHits: 92{{$}}

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

// Six own shapes: four emitted, overflow keeps arriving through the
// stable crossings, and the site still demotes.
function main() {
  function get(o) {
    return o.x;
  }
  function drive(f, arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += f(arr[i % 6]);
    return s;
  }
  var objs = [
    {x: 1}, {a: 0, x: 2}, {b: 0, x: 3},
    {c: 0, x: 4}, {d: 0, x: 5}, {e: 0, x: 6},
  ];
  print(drive(get, objs, 6000));
}
main();

// OUT: 21000
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (4 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP-NEXT: JIT ById sites: 1 recorded, 1 polymorphic, 0 chain, 1 megamorphic
// CNT: NumRecompiles: 1{{$}}
// CNT: NumByIdDemotions: 1{{$}}

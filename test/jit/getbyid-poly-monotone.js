/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=8 -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xdump-jitcode=3 %s | %FileCheck --check-prefix=DUMP %s
// RUN: %hermes -fno-inline -Xjit -Xjit-threshold=8 -Xjit-crash-on-error -Xjit-recompile-threshold=16 %s | %FileCheck --check-prefix=OUT %s
// REQUIRES: jit
// UNSUPPORTED: handle_san

// Version 1 is compiled WARM (threshold compile, not force), so the
// site gets the single-shape object specialization for `a`'s class;
// later shapes make it polymorphic, and version 2's chain must still
// contain version 1's shape.
//
// Sum: 50 reads of `a` (50); then 100 odd iterations read `a` (100)
// and the 100 even ones alternate others[0] and others[1]
// (50 * 2 + 50 * 3 = 250). Total 400.
function main() {
  function get(o) {
    return o.x;
  }
  function run(f, a, others) {
    var s = 0;
    for (var i = 0; i < 50; ++i)
      s += f(a);
    for (var i = 0; i < 200; ++i)
      s += f(i & 1 ? a : others[(i >> 1) & 1]);
    return s;
  }
  print(run(get, {x: 1}, [{p: 0, x: 2}, {q: 0, x: 3}]));
}
main();

// OUT: 400
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get'{{$}}
// DUMP: // Get from object specialization
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get'{{$}}
// DUMP: JIT compilation of FunctionID {{[0-9]+}}, 'get' (version 2)
// DUMP: // Get polymorphic chain (3 entries)
// DUMP: JIT successfully compiled FunctionID {{[0-9]+}}, 'get' (version 2)

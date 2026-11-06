/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// RUN: %hermes -fno-inline -Xjit=force -Xjit-crash-on-error -Xjit-recompile-threshold=16 -Xjit-emit-counters %s 2>&1 | %FileCheck %s
// REQUIRES: jit

// Retirement flips the old body's slot: a long activation of version 1
// keeps running after version 2 is installed (no OSR) and must stop
// counting. sum()'s GetById site sits in its own loop. Its first
// crossing (16th iteration) records two unemitted shapes, recompiles,
// and retires the running version 1, flipping its slot to the plain
// helper; the remaining 984 iterations run version 1 without counting.
// Without the flip they would cross 61 more times.
function main() {
  function sum(arr, n) {
    var s = 0;
    for (var i = 0; i < n; ++i)
      s += arr[i & 1].x;
    return s;
  }
  print(sum([{x: 1}, {y: 0, x: 2}], 1000));
}
main();

// CHECK: 1500
// CHECK: NumRecompileChecks: 1{{$}}
// CHECK: NumRecompiles: 1{{$}}
